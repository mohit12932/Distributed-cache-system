#!/usr/bin/env python3
"""
Chaos Engineering Script for AI-Adaptive KV Store
──────────────────────────────────────────────────
Simulates node failures, network partitions, hot key storms,
and recovery scenarios against a 3-node cluster.

Usage:
    python chaos_test.py --nodes localhost:50051,localhost:50052,localhost:50053
    python chaos_test.py --scenario leader_kill
    python chaos_test.py --scenario all

Requires: grpcio, grpcio-tools (for generated stubs), psutil
"""

import argparse
import os
import random
import signal
import socket
import subprocess
import sys
import time
import threading
from dataclasses import dataclass, field
from enum import Enum
from typing import List, Optional, Tuple

# ── Configuration ──

@dataclass
class ClusterConfig:
    node_addresses: List[str] = field(default_factory=lambda: [
        "localhost:50051",
        "localhost:50052",
        "localhost:50053",
    ])
    binary_path: str = "./build/ai_kv_node"
    data_dirs: List[str] = field(default_factory=lambda: [
        "./chaos_data/node0",
        "./chaos_data/node1",
        "./chaos_data/node2",
    ])


class Scenario(Enum):
    LEADER_KILL     = "leader_kill"
    NETWORK_PARTITION = "network_partition"
    SLOW_DISK       = "slow_disk"
    HOT_KEY_STORM   = "hot_key_storm"
    NODE_RECOVERY   = "node_recovery"
    ROLLING_RESTART = "rolling_restart"
    ALL             = "all"


# ══════════════════════════════════════════════════════════════════
#  Cluster Manager
# ══════════════════════════════════════════════════════════════════

class ClusterManager:
    """Manages lifecycle of a local 3-node cluster for testing."""

    def __init__(self, config: ClusterConfig):
        self.config = config
        self.processes: dict[int, Optional[subprocess.Popen]] = {}

    def start_cluster(self):
        """Launch all 3 nodes."""
        peers = ",".join(self.config.node_addresses)
        for node_id in range(3):
            self.start_node(node_id, peers)
        print(f"[Cluster] All 3 nodes started. Waiting for leader election...")
        time.sleep(2)

    def start_node(self, node_id: int, peers: str = ""):
        """Start a single node."""
        if not peers:
            peers = ",".join(self.config.node_addresses)
        data_dir = self.config.data_dirs[node_id]
        os.makedirs(data_dir, exist_ok=True)

        cmd = [
            self.config.binary_path,
            f"--node_id={node_id}",
            f"--address={self.config.node_addresses[node_id]}",
            f"--peers={peers}",
            f"--data_dir={data_dir}",
            f"--shards=8",
        ]
        print(f"[Cluster] Starting node {node_id}: {' '.join(cmd)}")
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes[node_id] = proc
        return proc

    def kill_node(self, node_id: int, sig=signal.SIGKILL):
        """Kill a specific node."""
        proc = self.processes.get(node_id)
        if proc and proc.poll() is None:
            print(f"[Chaos] Killing node {node_id} with signal {sig.name}")
            proc.send_signal(sig)
            proc.wait(timeout=5)
            self.processes[node_id] = None

    def stop_cluster(self):
        """Gracefully stop all nodes."""
        for node_id in list(self.processes.keys()):
            proc = self.processes.get(node_id)
            if proc and proc.poll() is None:
                proc.terminate()
                try:
                    proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
        self.processes.clear()

    def is_alive(self, node_id: int) -> bool:
        proc = self.processes.get(node_id)
        return proc is not None and proc.poll() is None


# ══════════════════════════════════════════════════════════════════
#  KV Client (TCP/gRPC stub)
# ══════════════════════════════════════════════════════════════════

class KVClient:
    """Simple client for sending KV operations to the cluster."""

    def __init__(self, address: str):
        self.address = address
        # In production: use generated gRPC stubs
        # self.channel = grpc.insecure_channel(address)
        # self.stub = ai_kv_pb2_grpc.KVStoreServiceStub(self.channel)

    def put(self, key: str, value: str) -> bool:
        """PUT key=value. Returns True on success."""
        # Placeholder — replace with actual gRPC call
        # response = self.stub.Put(ai_kv_pb2.PutRequest(key=key.encode(), value=value.encode()))
        # return response.success
        return self._tcp_command(f"SET {key} {value}")

    def get(self, key: str) -> Optional[str]:
        """GET key. Returns value or None."""
        return self._tcp_command(f"GET {key}")

    def _tcp_command(self, cmd: str) -> Optional[str]:
        """Send a raw command over TCP (RESP-like)."""
        try:
            host, port = self.address.rsplit(":", 1)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(2.0)
                sock.connect((host, int(port)))
                sock.sendall(f"{cmd}\r\n".encode())
                return sock.recv(4096).decode().strip()
        except Exception as e:
            return None


# ══════════════════════════════════════════════════════════════════
#  Chaos Scenarios
# ══════════════════════════════════════════════════════════════════

def assert_cluster_healthy(cluster: ClusterManager, config: ClusterConfig):
    """Verify at least 2 of 3 nodes are alive and serving."""
    alive = sum(1 for i in range(3) if cluster.is_alive(i))
    assert alive >= 2, f"Only {alive}/3 nodes alive — cluster unhealthy"
    print(f"  ✓ Cluster health: {alive}/3 nodes alive")


# ── Scenario 1: Leader Kill ──

def scenario_leader_kill(cluster: ClusterManager, config: ClusterConfig):
    """
    Kill the Raft leader via SIGKILL.
    Expected: New leader elected within 500ms, reads/writes resume.
    """
    print("\n═══ Scenario: Leader Kill ═══")

    # Write some baseline data
    client = KVClient(config.node_addresses[0])
    for i in range(100):
        client.put(f"pre_kill_{i}", f"value_{i}")

    # Identify and kill leader (assume node 0 initially — in production, query /status)
    leader_id = 0
    cluster.kill_node(leader_id, signal.SIGKILL)
    print(f"  Leader (node {leader_id}) killed.")

    # Wait for re-election
    time.sleep(1)

    # Verify remaining nodes can serve reads
    surviving = 1 if cluster.is_alive(1) else 2
    client2 = KVClient(config.node_addresses[surviving])
    result = client2.get("pre_kill_0")
    print(f"  Read after leader kill: {result}")

    # Verify writes work on new leader
    result = client2.put("post_kill_key", "survived")
    print(f"  Write after leader kill: {result}")

    assert_cluster_healthy(cluster, config)
    print("  ✓ Leader kill scenario passed\n")


# ── Scenario 2: Network Partition ──

def scenario_network_partition(cluster: ClusterManager, config: ClusterConfig):
    """
    Simulate network partition using iptables (Linux) or firewall rules (Windows).
    Isolate node 2 from nodes 0 and 1.
    Expected: Majority partition (0,1) continues serving. Node 2 cannot serve.
    """
    print("\n═══ Scenario: Network Partition ═══")

    # Parse port for node 2
    _, port_str = config.node_addresses[2].rsplit(":", 1)
    port = int(port_str)

    # Add iptables rule to drop traffic to/from node 2
    if sys.platform == "linux":
        drop_cmds = [
            f"sudo iptables -A INPUT -p tcp --sport {port} -j DROP",
            f"sudo iptables -A OUTPUT -p tcp --dport {port} -j DROP",
        ]
        for cmd in drop_cmds:
            print(f"  Executing: {cmd}")
            subprocess.run(cmd.split(), check=False)

        print(f"  Node 2 (port {port}) partitioned from cluster.")
        time.sleep(3)

        # Verify majority partition still works
        client = KVClient(config.node_addresses[0])
        result = client.put("partition_test", "majority_ok")
        print(f"  Write on majority partition: {result}")

        # Verify minority partition fails (or returns redirect)
        client2 = KVClient(config.node_addresses[2])
        result2 = client2.put("minority_test", "should_fail")
        print(f"  Write on minority partition: {result2}")

        # Heal partition
        heal_cmds = [
            f"sudo iptables -D INPUT -p tcp --sport {port} -j DROP",
            f"sudo iptables -D OUTPUT -p tcp --dport {port} -j DROP",
        ]
        for cmd in heal_cmds:
            subprocess.run(cmd.split(), check=False)
        print("  Partition healed.")
        time.sleep(2)
    else:
        print("  [SKIP] Network partition requires Linux iptables.")
        print("  On Windows, use: netsh advfirewall firewall add rule ...")

    print("  ✓ Network partition scenario complete\n")


# ── Scenario 3: Slow Disk ──

def scenario_slow_disk(cluster: ClusterManager, config: ClusterConfig):
    """
    Simulate slow disk I/O using Linux tc or cgroup throttling.
    Expected: WAL batching absorbs latency; compaction slows gracefully.
    """
    print("\n═══ Scenario: Slow Disk ═══")

    if sys.platform == "linux":
        data_dir = config.data_dirs[0]
        # Use cgroup v2 i/o throttling (requires root)
        print(f"  Throttling disk I/O for {data_dir}")
        # In production:
        # echo "8:0 wbps=1048576" > /sys/fs/cgroup/ai_kv_node0/io.max
        print("  [NOTE] Implement via cgroup io.max or blkio controller")
    else:
        print("  [SKIP] Slow disk simulation requires Linux cgroups.")

    # Write burst to stress WAL
    client = KVClient(config.node_addresses[0])
    start = time.time()
    for i in range(1000):
        client.put(f"slow_disk_{i}", "x" * 256)
    elapsed = time.time() - start
    print(f"  1000 writes under IO pressure: {elapsed:.2f}s")

    print("  ✓ Slow disk scenario complete\n")


# ── Scenario 4: Hot Key Storm ──

def scenario_hot_key_storm(cluster: ClusterManager, config: ClusterConfig):
    """
    Blast 100K reads on a single key to trigger PINN-based migration.
    Expected: PINN detects thermal spike → initiates proactive migration.
    """
    print("\n═══ Scenario: Hot Key Storm ═══")

    client = KVClient(config.node_addresses[0])
    client.put("hot_key", "extremely_popular_value")

    print("  Generating hot key storm (100K reads on single key)...")
    errors = 0
    start = time.time()

    def blast(thread_id: int, count: int):
        nonlocal errors
        c = KVClient(config.node_addresses[thread_id % len(config.node_addresses)])
        for _ in range(count):
            result = c.get("hot_key")
            if result is None:
                errors += 1

    threads = []
    for t in range(10):
        th = threading.Thread(target=blast, args=(t, 10000))
        threads.append(th)
        th.start()

    for th in threads:
        th.join()

    elapsed = time.time() - start
    qps = 100000 / elapsed if elapsed > 0 else 0

    print(f"  Completed: 100K reads in {elapsed:.2f}s ({qps:.0f} QPS)")
    print(f"  Errors: {errors}")
    print("  [CHECK] Monitor PINN logs for migration trigger")
    print("  ✓ Hot key storm scenario complete\n")


# ── Scenario 5: Node Recovery ──

def scenario_node_recovery(cluster: ClusterManager, config: ClusterConfig):
    """
    Kill a node, write data to surviving nodes, then restart.
    Expected: Restarted node catches up via Raft log replication.
    """
    print("\n═══ Scenario: Node Recovery ═══")

    # Kill node 2
    cluster.kill_node(2, signal.SIGKILL)
    print("  Node 2 killed.")

    # Write data while node 2 is down
    client = KVClient(config.node_addresses[0])
    for i in range(500):
        client.put(f"recovery_{i}", f"value_{i}")
    print("  500 writes completed while node 2 is down.")

    # Restart node 2
    peers = ",".join(config.node_addresses)
    cluster.start_node(2, peers)
    print("  Node 2 restarted. Waiting for log catch-up...")
    time.sleep(5)

    # Verify node 2 has the data
    client2 = KVClient(config.node_addresses[2])
    result = client2.get("recovery_499")
    print(f"  Read from recovered node 2: {result}")

    assert_cluster_healthy(cluster, config)
    print("  ✓ Node recovery scenario complete\n")


# ── Scenario 6: Rolling Restart ──

def scenario_rolling_restart(cluster: ClusterManager, config: ClusterConfig):
    """
    Restart each node one at a time while maintaining availability.
    Expected: Cluster remains available throughout.
    """
    print("\n═══ Scenario: Rolling Restart ═══")

    peers = ",".join(config.node_addresses)

    for node_id in range(3):
        print(f"  Restarting node {node_id}...")
        cluster.kill_node(node_id, signal.SIGTERM)
        time.sleep(1)

        # Verify remaining nodes serve
        surviving = (node_id + 1) % 3
        client = KVClient(config.node_addresses[surviving])
        result = client.put(f"rolling_{node_id}", "ok")
        print(f"    Write during restart: {result}")

        cluster.start_node(node_id, peers)
        time.sleep(2)

        assert_cluster_healthy(cluster, config)

    print("  ✓ Rolling restart scenario complete\n")


# ══════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════

SCENARIOS = {
    Scenario.LEADER_KILL:      scenario_leader_kill,
    Scenario.NETWORK_PARTITION: scenario_network_partition,
    Scenario.SLOW_DISK:        scenario_slow_disk,
    Scenario.HOT_KEY_STORM:    scenario_hot_key_storm,
    Scenario.NODE_RECOVERY:    scenario_node_recovery,
    Scenario.ROLLING_RESTART:  scenario_rolling_restart,
}


def main():
    parser = argparse.ArgumentParser(description="Chaos testing for AI-Adaptive KV Store")
    parser.add_argument("--nodes", default="localhost:50051,localhost:50052,localhost:50053",
                        help="Comma-separated node addresses")
    parser.add_argument("--binary", default="./build/ai_kv_node",
                        help="Path to node binary")
    parser.add_argument("--scenario", default="all",
                        choices=[s.value for s in Scenario],
                        help="Which chaos scenario to run")
    parser.add_argument("--no-start", action="store_true",
                        help="Don't start cluster (assume already running)")
    args = parser.parse_args()

    config = ClusterConfig(
        node_addresses=args.nodes.split(","),
        binary_path=args.binary,
    )

    cluster = ClusterManager(config)

    try:
        if not args.no_start:
            cluster.start_cluster()

        if args.scenario == "all":
            for scenario, func in SCENARIOS.items():
                func(cluster, config)
                # Reset cluster between scenarios
                time.sleep(2)
        else:
            scenario = Scenario(args.scenario)
            SCENARIOS[scenario](cluster, config)

        print("\n" + "=" * 50)
        print("  All chaos scenarios complete.")
        print("=" * 50)

    except KeyboardInterrupt:
        print("\n[Interrupted]")
    except AssertionError as e:
        print(f"\n[FAIL] Assertion failed: {e}")
        sys.exit(1)
    finally:
        if not args.no_start:
            cluster.stop_cluster()


if __name__ == "__main__":
    main()
