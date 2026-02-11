# AI-Adaptive Distributed Key-Value Store — Technical Blueprint

## 1. Executive Summary

A 3-node distributed KV store where a **Physics-Informed Neural Network (PINN)**
models data traffic as viscous fluid flow using Burgers' Equation, enabling
**predictive shard rebalancing** before latency spikes occur — not after.

The system uses:
- **Custom LSM-Tree** for sorted, write-optimized storage.
- **Raft Consensus** for linearizable replication across 3 nodes.
- **gRPC** for all inter-node communication (heartbeats, log replication, shard migration).
- **PINN Inference** running on a background thread to predict "thermal pressure" on each shard.

---

## 2. System Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Coordinator                              │
│  ┌──────────┐   ┌──────────────┐   ┌───────────────────────┐    │
│  │  gRPC    │──▶│  Raft Layer  │──▶│  Shard Manager        │    │
│  │  Server  │   │  (Leader/    │   │  (Consistent Hash     │    │
│  │          │   │   Follower)  │   │   Ring + PINN Trigger)│    │
│  └────┬─────┘   └──────┬───────┘   └──────────┬────────────┘    │
│       │                │                       │                 │
│       ▼                ▼                       ▼                 │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │                  Storage Engine (LSM-Tree)               │     │
│  │  ┌──────────┐  ┌──────────┐  ┌────────┐  ┌──────────┐  │     │
│  │  │ MemTable │  │   WAL    │  │ Level0 │  │ Level1+  │  │     │
│  │  │(SkipList)│  │(Append)  │  │ SSTs   │  │(Merged)  │  │     │
│  │  └──────────┘  └──────────┘  └────────┘  └──────────┘  │     │
│  └─────────────────────────────────────────────────────────┘     │
│       │                                                          │
│       ▼                                                          │
│  ┌─────────────────────────────────────────────────────────┐     │
│  │              ML Inference Layer                          │     │
│  │  ┌──────────────┐  ┌────────────────┐  ┌────────────┐  │     │
│  │  │Traffic Monitor│  │  PINN Model    │  │ Predictive │  │     │
│  │  │(Ring Buffer   │  │  (Burgers' PDE │  │ Sharder    │  │     │
│  │  │ of QPS/lat)   │  │   Constraint)  │  │ (Trigger)  │  │     │
│  │  └──────────────┘  └────────────────┘  └────────────┘  │     │
│  └─────────────────────────────────────────────────────────┘     │
└──────────────────────────────────────────────────────────────────┘
```

### Interaction Flow

1. **Client Request** → gRPC Server → Shard Manager routes to correct node.
2. **Write Path**: Raft Leader replicates LogEntry → Majority ACK → Apply to LSM-Tree.
3. **Read Path**: Leader reads from LSM-Tree (MemTable → L0 → L1 → ... → LN).
4. **Background**: Traffic Monitor samples QPS/latency per shard → feeds PINN every Δt.
5. **Prediction**: PINN outputs heat map → Predictive Sharder checks thresholds → triggers migration.
6. **Migration**: Write-Through Sync copies hot key ranges to cool shard via gRPC streaming.

---

## 3. Storage Engine: Custom LSM-Tree

### Components

| Component | Data Structure | Purpose |
|-----------|---------------|---------|
| MemTable | Lock-free skip list | In-memory sorted buffer for recent writes |
| WAL | Append-only file | Crash recovery — replayed on startup |
| SSTable | Block-indexed sorted file | Immutable on-disk sorted runs |
| Bloom Filter | Probabilistic set | Skip SSTable reads for absent keys |
| Compaction | Tiered / Leveled merge | Reduce read amplification |

### Write Path
```
PUT(k, v) → WAL.append(k, v) → MemTable.insert(k, v)
            │
            └─ if MemTable.size() > threshold:
                   freeze MemTable → flush to Level-0 SSTable
                   open new MemTable + WAL segment
```

### Read Path
```
GET(k) → MemTable.find(k)
         │ miss
         └→ Level-0 SSTables (newest first, bloom filter check)
            │ miss
            └→ Level-1+ SSTables (binary search within blocks)
```

### Compaction Strategy

Leveled compaction with size ratio R = 10:
- **Level 0**: Up to 4 overlapping SSTables (direct flush from MemTable).
- **Level L** (L ≥ 1): Non-overlapping SSTables, max size = R^L × base_size.
- Merge: Pick overlapping SSTables from L and L+1, merge-sort, write new SSTables to L+1.

---

## 4. Raft Consensus Protocol

### State Machine

Each node is in one of: **Follower**, **Candidate**, **Leader**.

```
                    timeout
    ┌─────────┐  ──────────▶  ┌───────────┐
    │ Follower │               │ Candidate │
    └─────────┘  ◀──────────  └─────┬─────┘
         ▲        discovers          │ wins election
         │        current leader     ▼
         │                    ┌──────────┐
         └────────────────────│  Leader   │
              steps down      └──────────┘
              (higher term)
```

### Leader Election (3-node cluster)

1. Follower's `election_timeout` (150–300ms randomized) expires.
2. Increments `current_term`, transitions to Candidate, votes for self.
3. Sends `RequestVote` RPC to peers.
4. Wins if receives majority (≥ 2 of 3) votes.
5. Begins sending `AppendEntries` heartbeats to maintain authority.

### Log Replication

1. Leader receives client write → appends `LogEntry{term, index, command}`.
2. Sends `AppendEntries` with new entries to all followers.
3. Follower appends if `prevLogIndex` and `prevLogTerm` match.
4. Once majority acknowledges → Leader commits → applies to state machine.
5. Committed index propagated via next heartbeat → followers apply.

### Safety Invariants
- **Election Safety**: At most one leader per term.
- **Log Matching**: If two entries share index + term, all preceding entries are identical.
- **Leader Completeness**: Committed entries appear in all future leaders' logs.

---

## 5. PINN Architecture — The Innovation

### 5.1 Problem Formulation

Model shard traffic density as a 1D viscous fluid. Each shard occupies a position
on a continuous domain $x \in [0, S]$ where $S$ = number of shards. The traffic
density $u(t, x)$ evolves according to **Burgers' Equation**:

$$
\frac{\partial u}{\partial t} + u \frac{\partial u}{\partial x} = \nu \frac{\partial^2 u}{\partial x^2}
$$

Where:
- $u(t, x)$ — traffic density (QPS normalized) at time $t$ on shard $x$
- $\nu$ — viscosity coefficient modeling natural load diffusion between shards
- The nonlinear convection term $u \cdot u_x$ captures the **self-reinforcing** nature
  of hot keys: high traffic attracts more traffic (cache warming, retries)
- The diffusion term $\nu \cdot u_{xx}$ models gradual load spreading

### 5.2 Neural Network Architecture

```
Input: [t, x]  ∈ ℝ²
    │
    ▼
┌──────────────────────┐
│  Dense(2 → 64, tanh) │  ← Fourier Feature Encoding optional
├──────────────────────┤
│  Dense(64 → 64, tanh)│  ← Residual connection
├──────────────────────┤
│  Dense(64 → 64, tanh)│  ← Residual connection
├──────────────────────┤
│  Dense(64 → 64, tanh)│
├──────────────────────┤
│  Dense(64 → 1, linear)│
└──────────────────────┘
    │
    ▼
Output: û(t, x) ∈ ℝ   (predicted heat / traffic density)
```

**Parameters**: ~12,800 weights — small enough for CPU inference at 1ms/forward-pass.

### 5.3 Loss Function

The total loss is a weighted sum of four terms:

$$
\mathcal{L}_{\text{total}} = \mathcal{L}_{\text{data}} + \lambda_r \mathcal{L}_{\text{PDE}} + \lambda_b \mathcal{L}_{\text{BC}} + \lambda_i \mathcal{L}_{\text{IC}}
$$

#### Data Fidelity Loss
Observed traffic telemetry from the Traffic Monitor ring buffer:

$$
\mathcal{L}_{\text{data}} = \frac{1}{N_d} \sum_{i=1}^{N_d} \left\| \hat{u}_\theta(t_i, x_i) - u_i^{\text{obs}} \right\|^2
$$

#### PDE Residual Loss (Physics Constraint)
Enforces Burgers' Equation at collocation points sampled via Latin Hypercube:

$$
\mathcal{L}_{\text{PDE}} = \frac{1}{N_r} \sum_{j=1}^{N_r} \left| \underbrace{\frac{\partial \hat{u}}{\partial t}}_{\text{temporal}} + \underbrace{\hat{u} \cdot \frac{\partial \hat{u}}{\partial x}}_{\text{nonlinear convection}} - \underbrace{\nu \frac{\partial^2 \hat{u}}{\partial x^2}}_{\text{diffusion}} \right|^2_{(t_j, x_j)}
$$

Derivatives $\hat{u}_t$, $\hat{u}_x$, $\hat{u}_{xx}$ are computed via **automatic differentiation**
through the network — no finite differences needed.

#### Boundary Condition Loss
Periodic boundaries (shard space wraps — consistent hash ring is circular):

$$
\mathcal{L}_{\text{BC}} = \frac{1}{N_b} \sum_{k=1}^{N_b} \left| \hat{u}(t_k, 0) - \hat{u}(t_k, S) \right|^2
$$

#### Initial Condition Loss
Anchors the model to the observed state at training window start $t_0$:

$$
\mathcal{L}_{\text{IC}} = \frac{1}{N_i} \sum_{m=1}^{N_i} \left| \hat{u}(t_0, x_m) - u^{\text{obs}}(t_0, x_m) \right|^2
$$

#### Hyperparameters
| Symbol | Value | Role |
|--------|-------|------|
| $\lambda_r$ | 1.0 | PDE residual weight |
| $\lambda_b$ | 0.1 | Boundary condition weight |
| $\lambda_i$ | 10.0 | Initial condition weight (strong anchoring) |
| $\nu$ | 0.01 | Viscosity (tuned per cluster) |
| $N_d$ | 512 | Data sample points per training window |
| $N_r$ | 2048 | Collocation points for PDE residual |

### 5.4 Training Schedule

- **Online incremental**: Every 5 seconds, retrain for 200 Adam iterations (lr=1e-3).
- **Warm-start**: Reuse previous weights — the traffic field changes smoothly.
- **Training data**: Sliding window of last 60 seconds of per-shard QPS telemetry.

### 5.5 Predictive Sharding Trigger

```
EVERY Δt = 1 second:
    FOR each shard s ∈ {0, 1, ..., S-1}:
        predicted_heat[s] = PINN.forward(t_now + T_horizon, s)

    hot_shards = { s : predicted_heat[s] > threshold_N }
    cool_shards = sorted by predicted_heat ascending

    FOR s_hot IN hot_shards:
        s_cool = cool_shards.pop_front()
        key_range = identify_hottest_keys(s_hot, top_k=100)
        initiate_write_through_sync(
            source = s_hot,
            target = s_cool,
            keys   = key_range
        )
        update_consistent_hash_ring(key_range → s_cool)
        log_migration_event(s_hot, s_cool, key_range, predicted_heat)
```

The prediction horizon $T_{\text{horizon}}$ = 10 seconds provides enough lead time for
background migration to complete before the predicted congestion materializes.

---

## 6. Thread Model & Safety

| Thread | Responsibility | Synchronization |
|--------|---------------|-----------------|
| gRPC I/O threads (N) | Handle client RPCs | Lock-free queue to Raft |
| Raft ticker (1) | Election timeouts, heartbeats | Mutex on RaftState |
| Raft applier (1) | Apply committed log → LSM-Tree | Condition variable |
| Compaction (1) | Background SSTable merging | Read-write lock on manifest |
| WAL writer (1) | Sequential WAL appends | Mutex + fdatasync batching |
| Traffic sampler (1) | Ring buffer telemetry | Atomic counters + SeqLock |
| PINN trainer (1) | Periodic model retraining | Double-buffered model swap |
| Shard migrator (1) | Background key range transfer | gRPC streaming + raft log |

**Double-buffered model swap**: The trainer writes to model_B while inference reads model_A.
On completion, an atomic pointer swap makes model_B live. Zero-copy, lock-free.

---

## 7. Chaos Engineering Approach

### Scenarios

| # | Scenario | Method | Expected Behavior |
|---|----------|--------|-------------------|
| 1 | Leader crash | SIGKILL leader process | New election in < 500ms |
| 2 | Network partition | iptables DROP between nodes | Minority side stops serving; majority elects new leader |
| 3 | Slow disk | `tc qdisc` + cgroup I/O throttle | WAL batching absorbs; compaction slows gracefully |
| 4 | Hot key storm | 100K QPS on single key | PINN predicts spike → migrates key range proactively |
| 5 | Node recovery | Restart killed node | Catches up via Raft log replication |
| 6 | Clock skew | `faketime` library | Election timeouts adapt; no split-brain |

---

## 8. Build & Dependencies

| Dependency | Version | Purpose |
|-----------|---------|---------|
| Abseil (absl) | 2024-LTS | Strings, hash maps, synchronization, time |
| gRPC + Protobuf | 1.60+ | Inter-node RPC |
| LevelDB (reference) | — | Bloom filter + block format inspiration |
| Eigen | 3.4 | Tensor ops for PINN forward pass |

### Build
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
```

### Run (3-node cluster)
```bash
./ai_kv_node --id=0 --peers="localhost:50051,localhost:50052,localhost:50053" --port=50051 --data-dir=./node0
./ai_kv_node --id=1 --peers="localhost:50051,localhost:50052,localhost:50053" --port=50052 --data-dir=./node1
./ai_kv_node --id=2 --peers="localhost:50051,localhost:50052,localhost:50053" --port=50053 --data-dir=./node2
```
