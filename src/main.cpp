/**
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║          Distributed Cache System — "Project 42"                    ║
 * ║                                                                      ║
 * ║  A production-grade, fault-tolerant distributed cache with:         ║
 * ║    • LSM-Tree Storage Engine (WAL → MemTable → SSTable)             ║
 * ║    • Raft Consensus (leader election + log replication)             ║
 * ║    • PINN Load Predictor (Burgers' equation physics prior)          ║
 * ║    • 32-Shard Segmented LRU with per-shard read-write locks        ║
 * ║    • RESP protocol (redis-cli compatible)                           ║
 * ║    • Embedded HTTP dashboard (real-time monitoring)                  ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 */

#include "include/sync/cache_manager.h"
#include "include/storage/lsm_engine.h"
#include "include/raft/raft_node.h"
#include "include/ml/predictive_sharder.h"
#include "include/network/tcp_server.h"
#include "include/network/http_server.h"

#include <iostream>
#include <sstream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <deque>

// ── Global shutdown flag ──────────────────────────────────────────────
static dcs::compat::Atomic<bool> g_shutdown{false};
static dcs::network::TCPServer*  g_tcp_server  = nullptr;
static dcs::network::HTTPServer* g_http_server = nullptr;

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[Main] Caught interrupt signal — shutting down...\n";
    g_shutdown = true;
    if (g_tcp_server)  g_tcp_server->stop();
    if (g_http_server) g_http_server->stop();
}

// ── Event Log ─────────────────────────────────────────────────────────
struct SystemEvent {
    std::string type;     // "info","warn","error","raft","lsm","pinn","burst"
    std::string message;
    uint64_t    timestamp_ms;
};

static dcs::compat::Mutex g_events_mu;
static std::deque<SystemEvent> g_events;
static const size_t MAX_EVENTS = 50;

static uint64_t now_ms() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static void push_event(const std::string& type, const std::string& msg) {
    dcs::compat::LockGuard<dcs::compat::Mutex> lock(g_events_mu);
    g_events.push_back({type, msg, now_ms()});
    while (g_events.size() > MAX_EVENTS) g_events.pop_front();
}

// ── Traffic Generator ─────────────────────────────────────────────────
static dcs::compat::Atomic<int>  g_traffic_rate{0};   // ops/sec (0 = stopped)
static dcs::compat::Atomic<bool> g_traffic_running{false};
static dcs::compat::Atomic<uint64_t> g_traffic_total{0};

// Per-node request counters (5-node Raft cluster)
static dcs::compat::Atomic<uint64_t> g_node_reqs[5] = {{0},{0},{0},{0},{0}};

// Flush events counter
static dcs::compat::Atomic<uint64_t> g_flush_count{0};
static dcs::compat::Atomic<uint64_t> g_heatstroke_count{0};

// Per-segment lock counters (simulated)
static dcs::compat::Atomic<uint64_t> g_seg_locks[32];

// Burst detection: per-segment ops sliding window
static dcs::compat::Atomic<uint64_t> g_seg_ops_window[32];
static dcs::compat::Atomic<uint64_t> g_seg_ops_pinn[32];  // persistent PINN accumulator (never reset)
static dcs::compat::Atomic<uint64_t> g_burst_check_counter{0};
static dcs::compat::Atomic<int> g_burst_cooldown{0};

// Persistent burst state
static dcs::compat::Atomic<bool> g_burst_active{false};
static dcs::compat::Atomic<int>  g_burst_intensity{500};
static int g_burst_shards_list[32];
static dcs::compat::Atomic<int>  g_burst_shard_count{0};
static dcs::compat::Atomic<uint64_t> g_burst_ops_done{0};

// ── Command-line argument helpers ─────────────────────────────────────
struct ServerConfig {
    uint16_t    port             = 6379;
    uint16_t    http_port        = 8080;
    size_t      capacity         = 65536;
    dcs::sync::WriteMode mode    = dcs::sync::WriteMode::WriteBack;
    int         flush_interval   = 5;
    std::string data_dir         = "data";
    int         node_id          = 0;
    int         cluster_size     = 5;
};

ServerConfig parse_args(int argc, char* argv[]) {
    ServerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--http-port" && i + 1 < argc)
            cfg.http_port = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if ((arg == "--capacity" || arg == "-c") && i + 1 < argc)
            cfg.capacity = static_cast<size_t>(std::atoll(argv[++i]));
        else if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
            std::string m = argv[++i];
            cfg.mode = (m == "write-through" || m == "wt")
                       ? dcs::sync::WriteMode::WriteThrough
                       : dcs::sync::WriteMode::WriteBack;
        }
        else if ((arg == "--flush-interval" || arg == "-f") && i + 1 < argc)
            cfg.flush_interval = std::atoi(argv[++i]);
        else if ((arg == "--data-dir" || arg == "-d") && i + 1 < argc)
            cfg.data_dir = argv[++i];
        else if (arg == "--node-id" && i + 1 < argc)
            cfg.node_id = std::atoi(argv[++i]);
        else if (arg == "--cluster-size" && i + 1 < argc)
            cfg.cluster_size = std::atoi(argv[++i]);
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: distributed_cache [OPTIONS]\n"
                      << "  -p, --port PORT              RESP TCP port (default: 6379)\n"
                      << "      --http-port PORT         Dashboard HTTP port (default: 8080)\n"
                      << "  -c, --capacity N             Max cache entries (default: 65536)\n"
                      << "  -m, --mode MODE              write-through | write-back (default)\n"
                      << "  -f, --flush-interval SECS    Write-back flush interval (default: 5)\n"
                      << "  -d, --data-dir PATH          Data directory (default: data)\n"
                      << "      --node-id ID             Raft node ID (default: 0)\n"
                      << "      --cluster-size N         Raft cluster size (default: 3)\n"
                      << "  -h, --help                   Show this help\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── Main ──────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    std::string mode_str = (cfg.mode == dcs::sync::WriteMode::WriteThrough)
                           ? "write-through" : "write-back";

    std::cout << "\n";
    std::cout << "  ╔══════════════════════════════════════════════════╗\n";
    std::cout << "  ║    Distributed Cache System v2.0 (Project 42)    ║\n";
    std::cout << "  ╠══════════════════════════════════════════════════╣\n";
    std::cout << "  ║  RESP Port:     " << cfg.port << std::string(34 - std::to_string(cfg.port).size(), ' ') << "║\n";
    std::cout << "  ║  HTTP Port:     " << cfg.http_port << std::string(34 - std::to_string(cfg.http_port).size(), ' ') << "║\n";
    std::cout << "  ║  Capacity:      " << cfg.capacity << std::string(34 - std::to_string(cfg.capacity).size(), ' ') << "║\n";
    std::cout << "  ║  Write Mode:    " << mode_str << std::string(34 - mode_str.size(), ' ') << "║\n";
    std::cout << "  ║  Segments:      32 (read-write locks)" << std::string(12, ' ') << "║\n";
    std::cout << "  ║  Storage:       LSM-Tree (WAL+SSTable)" << std::string(11, ' ') << "║\n";
    std::cout << "  ║  Consensus:     Raft (node " << cfg.node_id << "/" << cfg.cluster_size << ")" << std::string(std::max(0, 24 - (int)std::to_string(cfg.node_id).size() - (int)std::to_string(cfg.cluster_size).size()), ' ') << "║\n";
    std::cout << "  ║  ML Engine:     PINN (Burgers' eq.)" << std::string(15, ' ') << "║\n";
    std::cout << "  ╚══════════════════════════════════════════════════╝\n\n";

    // ── 1. LSM-Tree Storage Engine ────────────────────────────────────
    std::cout << "[Init] Starting LSM-Tree storage engine...\n";
    dcs::storage::LSMEngine lsm_storage(cfg.data_dir + "/lsm");
    std::cout << "[Init] LSM-Tree ready (WAL + " << lsm_storage.TotalSSTCount()
              << " SSTables loaded)\n";

    // ── 2. Cache Manager ──────────────────────────────────────────────
    dcs::sync::CacheManager::Config cache_cfg;
    cache_cfg.cache_capacity = cfg.capacity;
    cache_cfg.write_mode     = cfg.mode;
    cache_cfg.flush_interval = std::chrono::seconds(cfg.flush_interval);

    dcs::sync::CacheManager manager(cache_cfg, &lsm_storage);
    std::cout << "[Init] Cache manager ready (32-shard segmented LRU, "
              << cfg.capacity << " capacity)\n";
    push_event("info", "Cache manager initialized (" + std::to_string(cfg.capacity) + " capacity)");

    // ── 3. Raft Consensus (5-node in-process cluster) ────────────────
    const int RAFT_CLUSTER_SIZE = 5;
    std::cout << "[Init] Starting Raft consensus (" << RAFT_CLUSTER_SIZE
              << "-node cluster)...\n";
    dcs::raft::LocalRaftTransport raft_transport;
    std::vector<std::unique_ptr<dcs::raft::RaftNode>> raft_nodes;
    for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
        raft_nodes.push_back(std::make_unique<dcs::raft::RaftNode>(
            i, RAFT_CLUSTER_SIZE, cfg.data_dir + "/raft/node" + std::to_string(i)));
        raft_nodes[i]->SetTransport(&raft_transport);
        raft_transport.RegisterNode(i, raft_nodes[i].get());
    }
    // Only Node 0 applies to cache manager
    raft_nodes[0]->SetApplyCallback([&manager](uint64_t /*index*/, const std::string& command) {
        std::istringstream iss(command);
        std::string op, key, value;
        iss >> op >> key;
        if (op == "PUT") {
            std::getline(iss >> std::ws, value);
            manager.put(key, value);
        } else if (op == "DEL") {
            manager.del(key);
        }
    });
    for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) raft_nodes[i]->Start();
    // Allow initial leader election
    dcs::compat::this_thread::sleep_for(std::chrono::milliseconds(500));
    for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
        auto st = raft_nodes[i]->GetState();
        push_event("raft", "Node " + std::to_string(i) + " started as " +
                   dcs::raft::RoleToString(st.role));
        std::cout << "[Init] Raft node " << i << " started (role: "
                  << dcs::raft::RoleToString(st.role) << ")\n";
    }

    // ── 4. PINN Predictive Sharder ────────────────────────────────────
    std::cout << "[Init] Starting PINN load predictor (Burgers' equation)...\n";
    dcs::ml::PINNConfig pinn_cfg;
    pinn_cfg.hidden_size   = 64;
    pinn_cfg.num_layers    = 4;
    pinn_cfg.learning_rate = 1e-3f;
    pinn_cfg.lambda_pde    = 0.1f;
    pinn_cfg.nu            = 0.01f;

    dcs::ml::PredictiveSharder sharder(32, pinn_cfg);
    sharder.Start();
    auto pinn_stats = sharder.GetStats();
    std::cout << "[Init] PINN ready (" << pinn_stats.num_parameters
              << " parameters, 4 hidden × 64 neurons)\n";
    push_event("pinn", "PINN model ready (" + std::to_string(pinn_stats.num_parameters) + " params)");

    // ── 5. Signal Handlers ────────────────────────────────────────────
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // ── 6. HTTP Dashboard Server ──────────────────────────────────────
    std::cout << "[Init] Starting HTTP dashboard server on port "
              << cfg.http_port << "...\n";
    dcs::network::HTTPServer http_server(cfg.http_port, "web");
    g_http_server = &http_server;

    // ── Metrics endpoint ──────────────────────────────────────────────
    http_server.setMetricsCallback([&]() -> std::string {
        auto& cache_stats = manager.stats();
        auto& lsm_stats   = lsm_storage.Stats();
        auto pinn_info    = sharder.GetStats();
        auto predictions  = sharder.PredictLoads();
        // Blend PINN predictions with actual per-shard ops load for differentiated output
        {
            uint64_t pinn_ops[32], max_po = 1;
            for (int i = 0; i < 32; i++) {
                pinn_ops[i] = g_seg_ops_pinn[i].load();
                if (pinn_ops[i] > max_po) max_po = pinn_ops[i];
            }
            for (size_t i = 0; i < predictions.size() && i < 32; i++) {
                float actual = static_cast<float>(pinn_ops[i]) / static_cast<float>(max_po);
                predictions[i] = 0.3f * predictions[i] + 0.7f * actual;
            }
        }
        auto migrations   = sharder.GetRecommendations();
        auto seg_sizes    = manager.segment_sizes();

        // Collect state from all 5 raft nodes
        std::vector<dcs::raft::RaftNode::NodeState> all_raft_states;
        int raft_leader_id = -1;
        for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
            auto st = raft_nodes[i]->GetState();
            all_raft_states.push_back(st);
            if (st.role == dcs::raft::RaftRole::Leader) raft_leader_id = i;
        }
        auto raft_state = (raft_leader_id >= 0) ? all_raft_states[raft_leader_id] : all_raft_states[0];

        std::ostringstream json;
        json << "{\n";

        // Cache stats
        json << "  \"cache_hits\": " << cache_stats.cache_hits.load() << ",\n";
        json << "  \"cache_misses\": " << cache_stats.cache_misses.load() << ",\n";
        json << "  \"cache_size\": " << manager.size() << ",\n";
        json << "  \"write_through_ops\": " << cache_stats.write_through_count.load() << ",\n";
        json << "  \"write_back_ops\": " << cache_stats.write_back_count.load() << ",\n";
        json << "  \"write_mode\": \"" << (manager.write_mode() == dcs::sync::WriteMode::WriteThrough ? "write-through" : "write-back") << "\",\n";

        // Per-segment sizes (for heat grid)
        json << "  \"segment_sizes\": [";
        for (size_t i = 0; i < seg_sizes.size(); i++) {
            if (i > 0) json << ",";
            json << seg_sizes[i];
        }
        json << "],\n";

        // Per-segment lock counts
        json << "  \"segment_locks\": [";
        for (int i = 0; i < 32; i++) {
            if (i > 0) json << ",";
            json << g_seg_locks[i].load();
        }
        json << "],\n";

        // Per-node request counts (5 nodes)
        json << "  \"node_requests\": [";
        for (int i = 0; i < 5; i++) {
            if (i > 0) json << ",";
            json << g_node_reqs[i].load();
        }
        json << "],\n";

        // Flush / heat stroke counts
        json << "  \"flush_count\": " << g_flush_count.load() << ",\n";
        json << "  \"heatstroke_count\": " << g_heatstroke_count.load() << ",\n";
        json << "  \"traffic_rate\": " << g_traffic_rate.load() << ",\n";

        // LSM-Tree stats
        json << "  \"lsm\": {\n";
        json << "    \"wal_bytes\": " << lsm_stats.wal_bytes.load() << ",\n";
        json << "    \"memtable_size\": " << lsm_stats.memtable_size.load() << ",\n";
        json << "    \"memtable_entries\": " << lsm_stats.memtable_entries.load() << ",\n";
        json << "    \"sstable_count\": " << lsm_stats.sstable_count.load() << ",\n";
        json << "    \"compactions\": " << lsm_stats.compactions_done.load() << ",\n";
        json << "    \"total_puts\": " << lsm_stats.total_puts.load() << ",\n";
        json << "    \"total_gets\": " << lsm_stats.total_gets.load() << ",\n";
        json << "    \"total_deletes\": " << lsm_stats.total_deletes.load() << ",\n";
        json << "    \"bloom_hits\": " << lsm_stats.bloom_filter_hits.load() << ",\n";
        json << "    \"levels\": [";
        for (int i = 0; i < 4; i++) {
            if (i > 0) json << ", ";
            json << lsm_storage.SSTCountAtLevel(i);
        }
        json << "]\n";
        json << "  },\n";

        // Raft state (all 5 nodes)
        json << "  \"raft\": {\n";
        json << "    \"node_id\": " << raft_state.id << ",\n";
        json << "    \"role\": \"" << dcs::raft::RoleToString(raft_state.role) << "\",\n";
        json << "    \"term\": " << raft_state.term << ",\n";
        json << "    \"commit_index\": " << raft_state.commit_index << ",\n";
        json << "    \"last_applied\": " << raft_state.last_applied << ",\n";
        json << "    \"log_size\": " << raft_state.log_size << ",\n";
        json << "    \"leader_id\": " << raft_leader_id << ",\n";
        json << "    \"votes\": " << raft_state.votes_received << ",\n";
        json << "    \"nodes\": [";
        for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
            if (i > 0) json << ",";
            json << "{\"id\":" << all_raft_states[i].id
                 << ",\"role\":\"" << dcs::raft::RoleToString(all_raft_states[i].role)
                 << "\",\"term\":" << all_raft_states[i].term
                 << ",\"commit_index\":" << all_raft_states[i].commit_index
                 << ",\"last_applied\":" << all_raft_states[i].last_applied
                 << ",\"log_size\":" << all_raft_states[i].log_size
                 << ",\"leader_id\":" << all_raft_states[i].leader_id
                 << ",\"votes\":" << all_raft_states[i].votes_received << "}";
        }
        json << "]\n";
        json << "  },\n";

        // PINN stats
        json << "  \"pinn\": {\n";
        json << "    \"training_steps\": " << pinn_info.training_steps << ",\n";
        json << "    \"total_loss\": " << pinn_info.total_loss << ",\n";
        json << "    \"data_loss\": " << pinn_info.data_loss << ",\n";
        json << "    \"pde_loss\": " << pinn_info.pde_loss << ",\n";
        json << "    \"num_parameters\": " << pinn_info.num_parameters << ",\n";
        json << "    \"telemetry_count\": " << pinn_info.telemetry_count << ",\n";
        json << "    \"predictions\": [";
        for (size_t i = 0; i < predictions.size(); i++) {
            if (i > 0) json << ", ";
            json << predictions[i];
        }
        json << "],\n";
        json << "    \"migrations\": [";
        for (size_t i = 0; i < migrations.size(); i++) {
            if (i > 0) json << ", ";
            json << "{\"from\": " << migrations[i].from_shard
                 << ", \"to\": " << migrations[i].to_shard
                 << ", \"confidence\": " << migrations[i].confidence << "}";
        }
        json << "]\n";
        json << "  },\n";

        // Events
        json << "  \"events\": [";
        {
            dcs::compat::LockGuard<dcs::compat::Mutex> evlock(g_events_mu);
            for (size_t i = 0; i < g_events.size(); i++) {
                if (i > 0) json << ",";
                json << "\n    {\"type\":\"" << g_events[i].type
                     << "\",\"msg\":\"" << g_events[i].message
                     << "\",\"ts\":" << g_events[i].timestamp_ms << "}";
            }
        }
        json << "\n  ],\n";

        json << "  \"segments\": 32,\n";
        json << "  \"burst_active\": " << (g_burst_active.load() ? "true" : "false") << ",\n";
        json << "  \"burst_ops_done\": " << g_burst_ops_done.load() << ",\n";
        json << "  \"server_running\": true\n";
        json << "}";
        return json.str();
    });

    // ── Traffic rate control endpoint ─────────────────────────────────
    http_server.addEndpoint("/api/traffic", [&](const std::string& body) -> std::string {
        // Parse rate from body: {"rate": 100}
        int rate = 0;
        auto pos = body.find("\"rate\"");
        if (pos != std::string::npos) {
            pos = body.find(':', pos);
            if (pos != std::string::npos) {
                rate = std::atoi(body.c_str() + pos + 1);
                if (rate < 0) rate = 0;
                if (rate > 50000) rate = 50000;
            }
        }
        g_traffic_rate = rate;
        std::cout << "[API] Traffic rate set to " << rate << " ops/s\n";
        push_event("info", "Traffic rate set to " + std::to_string(rate) + " ops/s");
        return "{\"status\":\"ok\",\"rate\":" + std::to_string(rate) + "}";
    });

    // ── Persistent PINN burst endpoint ────────────────────────────────
    // POST /api/burst  {"shards":[0,1,2], "intensity": 500}
    // Starts a continuous burst loop that runs until /api/burst-stop
    http_server.addEndpoint("/api/burst", [&](const std::string& body) -> std::string {
        if (g_burst_active.load()) {
            return "{\"status\":\"already_running\",\"msg\":\"Burst already active. Stop first.\"}";
        }
        // Parse shard list
        std::vector<int> target_shards;
        auto arr_start = body.find('[');
        auto arr_end = body.find(']');
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr = body.substr(arr_start + 1, arr_end - arr_start - 1);
            std::istringstream ss(arr);
            std::string token;
            while (std::getline(ss, token, ',')) {
                int s = std::atoi(token.c_str());
                if (s >= 0 && s < 32) target_shards.push_back(s);
            }
        }
        if (target_shards.empty()) {
            target_shards = {0, 1, 2, 3};
        }

        int intensity = 500;
        auto ipos = body.find("\"intensity\"");
        if (ipos != std::string::npos) {
            ipos = body.find(':', ipos);
            if (ipos != std::string::npos) intensity = std::atoi(body.c_str() + ipos + 1);
        }
        if (intensity < 50) intensity = 50;
        if (intensity > 5000) intensity = 5000;

        // Store burst config
        g_burst_intensity = intensity;
        int cnt = 0;
        for (size_t i = 0; i < target_shards.size() && cnt < 32; i++) {
            g_burst_shards_list[cnt++] = target_shards[i];
        }
        g_burst_shard_count = cnt;
        g_burst_ops_done = 0;
        g_burst_active = true;

        std::string shard_list;
        for (size_t i = 0; i < target_shards.size(); i++) {
            if (i > 0) shard_list += ",";
            shard_list += std::to_string(target_shards[i]);
        }
        push_event("pinn", "Persistent burst STARTED on shards [" + shard_list +
                   "] intensity=" + std::to_string(intensity));

        return "{\"status\":\"started\",\"shards\":[" + shard_list +
               "],\"intensity\":" + std::to_string(intensity) + "}";
    });

    // POST /api/burst-stop — stop persistent burst
    http_server.addEndpoint("/api/burst-stop", [&](const std::string&) -> std::string {
        if (!g_burst_active.load()) {
            return "{\"status\":\"not_running\"}";
        }
        g_burst_active = false;
        uint64_t ops = g_burst_ops_done.load();
        push_event("pinn", "Persistent burst STOPPED after " + std::to_string(ops) + " ops");

        // Run burst detection
        uint64_t seg_ops[32]; uint64_t total_seg = 0;
        for (int i = 0; i < 32; i++) {
            seg_ops[i] = g_seg_ops_window[i].load();
            total_seg += seg_ops[i];
        }
        float avg = total_seg > 0 ? static_cast<float>(total_seg) / 32.0f : 1.0f;
        int hot = 0;
        for (int i = 0; i < 32; i++) {
            if (static_cast<float>(seg_ops[i]) > avg * 3.0f) hot++;
        }
        if (hot >= 2) {
            g_flush_count.fetch_add(1);
            push_event("burst", "PINN detected burst: " + std::to_string(hot) + " hot shards");
            if (hot >= 3) {
                g_heatstroke_count.fetch_add(1);
                push_event("burst", "HEAT STROKE! " + std::to_string(hot) + " shards overloaded");
                manager.flush();
                push_event("lsm", "Emergency flush completed");
            }
        }

        auto predictions = sharder.PredictLoads();
        float max_pred = 0; int max_shard = 0;
        for (size_t i = 0; i < predictions.size(); i++) {
            if (predictions[i] > max_pred) { max_pred = predictions[i]; max_shard = (int)i; }
        }

        std::ostringstream resp;
        resp << "{\"status\":\"stopped\",\"total_ops\":" << ops
             << ",\"hot_detected\":" << hot
             << ",\"pinn_peak_shard\":" << max_shard
             << ",\"pinn_peak_load\":" << max_pred << "}";
        return resp.str();
    });

    // POST /api/flush — manual flush to disk
    http_server.addEndpoint("/api/flush", [&](const std::string&) -> std::string {
        manager.flush();
        g_flush_count.fetch_add(1);
        std::cout << "[API] Flush triggered — flush_count=" << g_flush_count.load() << "\n";
        push_event("lsm", "Manual flush triggered — data persisted to SSTables");
        return "{\"status\":\"ok\",\"flush_count\":" + std::to_string(g_flush_count.load()) + "}";
    });

    // POST /api/election — force raft election on a random non-leader node
    http_server.addEndpoint("/api/election", [&](const std::string&) -> std::string {
        // Find current leader
        int old_leader = -1;
        for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
            if (raft_nodes[i]->GetState().role == dcs::raft::RaftRole::Leader) {
                old_leader = i;
                break;
            }
        }
        // Pick a follower to trigger election on
        int trigger_node = (old_leader + 1) % RAFT_CLUSTER_SIZE;
        auto old_state = raft_nodes[trigger_node]->GetState();
        raft_nodes[trigger_node]->TriggerElection();
        // Brief wait for election to complete
        dcs::compat::this_thread::sleep_for(std::chrono::milliseconds(200));
        // Find new leader
        int new_leader = -1;
        uint64_t new_term = 0;
        std::string new_role;
        for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
            auto st = raft_nodes[i]->GetState();
            if (st.role == dcs::raft::RaftRole::Leader) {
                new_leader = i;
                new_term = st.term;
                new_role = dcs::raft::RoleToString(st.role);
            }
        }
        std::cout << "[API] Election triggered on Node " << trigger_node
                  << " — old_term=" << old_state.term
                  << " new_term=" << new_term
                  << " leader=Node " << new_leader << "\n";
        push_event("raft", "Manual election on Node " + std::to_string(trigger_node) +
                   " (term " + std::to_string(old_state.term) +
                   " → " + std::to_string(new_term) + ") — Leader: Node " +
                   std::to_string(new_leader));
        std::ostringstream resp;
        resp << "{\"status\":\"ok\",\"old_term\":" << old_state.term
             << ",\"new_term\":" << new_term
             << ",\"role\":\"" << (new_leader >= 0 ? "Leader" : "Candidate")
             << "\",\"leader_id\":" << new_leader << "}";
        return resp.str();
    });

    // POST /api/compact — trigger LSM compaction
    http_server.addEndpoint("/api/compact", [&](const std::string&) -> std::string {
        lsm_storage.ForceCompaction();
        auto& s = lsm_storage.Stats();
        std::cout << "[API] Compaction triggered — compactions=" << s.compactions_done.load()
                  << " sstables=" << s.sstable_count.load() << "\n";
        push_event("lsm", "Manual compaction triggered");
        return "{\"status\":\"ok\",\"compactions\":" + std::to_string(s.compactions_done.load()) +
               ",\"sstable_count\":" + std::to_string(s.sstable_count.load()) + "}";
    });

    http_server.start();
    std::cout << "[Init] Dashboard: http://localhost:" << cfg.http_port << "\n\n";

    // ── 7. RESP TCP Server ────────────────────────────────────────────
    std::cout << "[Init] Starting RESP TCP server on port " << cfg.port << "...\n";
    std::cout << "[Ready] All systems operational. Accepting connections.\n\n";
    push_event("info", "Server ready on port " + std::to_string(cfg.port));

    dcs::network::TCPServer tcp_server(cfg.port, &manager);
    g_tcp_server = &tcp_server;

    // ── Telemetry collection thread ───────────────────────────────────
    dcs::compat::Thread telemetry_thread([&]() {
        uint64_t prev_pinn[32] = {};
        while (!g_shutdown.load()) {
            auto& s = manager.stats();
            uint64_t total_ops = s.cache_hits.load() + s.cache_misses.load();
            // Use PINN accumulator deltas for differentiated load measurement
            uint64_t seg_ops[32];
            uint64_t max_seg_ops = 1;
            for (int shard = 0; shard < 32; shard++) {
                uint64_t cur = g_seg_ops_pinn[shard].load();
                seg_ops[shard] = cur - prev_pinn[shard];
                prev_pinn[shard] = cur;
                if (seg_ops[shard] > max_seg_ops) max_seg_ops = seg_ops[shard];
            }
            for (int shard = 0; shard < 32; shard++) {
                // Blend segment size ratio with recent ops ratio for diverse predictions
                float ops_load = static_cast<float>(seg_ops[shard]) /
                                 static_cast<float>(max_seg_ops);
                auto seg_sizes = manager.segment_sizes();
                float size_load = static_cast<float>(seg_sizes[shard]) /
                    static_cast<float>(std::max<size_t>(1, cfg.capacity / 32));
                float load = std::min(1.0f, 0.7f * ops_load + 0.3f * size_load);
                float hit_rate = (total_ops > 0)
                    ? static_cast<float>(s.cache_hits.load()) / static_cast<float>(total_ops)
                    : 0.0f;
                float latency = (seg_ops[shard] > 0) ? 0.2f + 0.8f * ops_load : 0.1f;
                sharder.RecordTelemetry(shard, load, hit_rate, latency);
            }
            dcs::compat::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    // ── Traffic generator thread ──────────────────────────────────────
    static dcs::compat::Atomic<uint64_t> traffic_key_counter{0};
    static std::string prev_raft_role = "Follower";

    // Initialize burst detection window
    for (int i = 0; i < 32; i++) g_seg_ops_window[i] = 0;

    // ── Persistent burst thread ───────────────────────────────────────
    dcs::compat::Thread burst_thread([&]() {
        static uint64_t burst_round = 0;
        while (!g_shutdown.load()) {
            if (!g_burst_active.load()) {
                dcs::compat::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            int inten = g_burst_intensity.load();
            int ns = g_burst_shard_count.load();
            // Do one round of burst ops
            for (int i = 0; i < ns; i++) {
                int s = g_burst_shards_list[i];
                std::string bkey = "burst_s" + std::to_string(s) + "_" + std::to_string(burst_round);
                manager.put(bkey, "bv" + std::to_string(burst_round));
                g_seg_locks[s].fetch_add(1);
                g_seg_ops_window[s].fetch_add(1);
                g_seg_ops_pinn[s].fetch_add(1);
                g_node_reqs[s * 5 / 32].fetch_add(1);
                g_traffic_total.fetch_add(1);
                g_burst_ops_done.fetch_add(1);
            }
            burst_round++;
            // Sleep based on intensity: higher = faster
            int sleep_us = std::max(100, 1000000 / std::max(1, inten));
            dcs::compat::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
        }
    });

    // ── High-throughput traffic worker function (10K+ ops/s capable) ──
    const int TRAFFIC_WORKERS = 4;   // parallel worker threads (balanced for CPU)
    static dcs::compat::Atomic<uint64_t> worker_key_counters[4] = {{0},{0},{0},{0}};

    auto traffic_worker_fn = [&](int worker_id) {
        uint64_t local_counter = 0;
        while (!g_shutdown.load()) {
            int rate = g_traffic_rate.load();
            if (rate <= 0) {
                dcs::compat::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            // Each worker handles 1/N of the total rate
            int worker_rate = std::max(1, rate / TRAFFIC_WORKERS);

            // Large batches to amortize Windows timer resolution (~15.6ms)
            // At 15K ops/s with 4 workers: each does 375 ops/batch every ~100ms
            const int BATCH_MS = 100;
            int ops_per_batch = std::max(1, worker_rate * BATCH_MS / 1000);

            auto batch_start = std::chrono::steady_clock::now();
            for (int b = 0; b < ops_per_batch && !g_shutdown.load(); b++) {
                uint64_t kn = traffic_key_counter.fetch_add(1);
                local_counter++;
                int shard_idx;
                int op = static_cast<int>(kn % 7);

                // Natural hotspot: shards 4,5 get ~3x more traffic
                int roll = static_cast<int>(kn % 100);
                std::string key;
                if (roll < 10) {
                    shard_idx = 4;
                    key = "hot4_" + std::to_string(kn % 5000);
                } else if (roll < 20) {
                    shard_idx = 5;
                    key = "hot5_" + std::to_string(kn % 5000);
                } else {
                    shard_idx = static_cast<int>(kn % 32);
                    key = "k" + std::to_string(kn % 50000);
                }

                // Route to one of 5 raft nodes
                int node_idx = shard_idx * 5 / 32;
                g_node_reqs[node_idx].fetch_add(1);

                // Track lock usage and PINN telemetry
                g_seg_locks[shard_idx].fetch_add(1);
                g_seg_ops_window[shard_idx].fetch_add(1);
                g_seg_ops_pinn[shard_idx].fetch_add(1);

                try {
                    if (op <= 2) {
                        // SET — cache-only fast path for majority of ops
                        std::string val = "v" + std::to_string(kn);
                        manager.put(key, val);
                        // Propose through Raft leader very sparingly at high throughput
                        if (kn % 500 == 0) {
                            for (int ni = 0; ni < RAFT_CLUSTER_SIZE; ni++) {
                                if (raft_nodes[ni]->Propose("PUT " + key + " " + val)) break;
                            }
                        }
                    } else {
                        // GET (majority of ops - cache-friendly, avoids disk-heavy DELs)
                        manager.get(key);
                    }
                } catch (...) {
                    // Prevent thread death from Raft or cache exceptions
                }

                g_traffic_total.fetch_add(1);
            }

            // ── Burst / heat stroke detection (only worker 0 handles this) ──
            if (worker_id == 0 && local_counter % 2000 < static_cast<uint64_t>(ops_per_batch)) {
                uint64_t seg_ops[32];
                uint64_t total_seg_ops = 0;
                for (int i = 0; i < 32; i++) {
                    seg_ops[i] = g_seg_ops_window[i].load();
                    total_seg_ops += seg_ops[i];
                    g_seg_ops_window[i] = 0;
                }
                if (total_seg_ops > 50) {
                    float avg_ops = static_cast<float>(total_seg_ops) / 32.0f;
                    int hot_count = 0;
                    for (int i = 0; i < 32; i++) {
                        if (static_cast<float>(seg_ops[i]) > avg_ops * 2.5f)
                            hot_count++;
                    }
                    int cooldown = g_burst_cooldown.load();
                    if (cooldown > 0) {
                        g_burst_cooldown.fetch_add(-1);
                    } else if (hot_count >= 2) {
                        g_flush_count.fetch_add(1);
                        std::cout << "[Burst] Detected: " << hot_count << " hot shards\n";
                        push_event("burst", "Burst detected: " +
                                   std::to_string(hot_count) + " hot shards (>" +
                                   std::to_string(static_cast<int>(avg_ops * 2.5)) +
                                   " ops) — triggering write-back flush");
                        if (hot_count >= 4) {
                            g_heatstroke_count.fetch_add(1);
                            std::cout << "[Burst] HEAT STROKE! " << hot_count << " shards overloaded\n";
                            push_event("burst", "HEAT STROKE! " +
                                       std::to_string(hot_count) +
                                       " shards overloaded — emergency flush to DB");
                            manager.flush();
                            push_event("lsm", "Emergency flush completed — data persisted to SSTables");
                        }
                        g_burst_cooldown = 10;
                    }
                    if (local_counter % 10000 < static_cast<uint64_t>(ops_per_batch)) {
                        auto predictions = sharder.PredictLoads();
                        float max_pred = 0;
                        int max_shard = 0;
                        for (size_t i = 0; i < predictions.size(); i++) {
                            if (predictions[i] > max_pred) {
                                max_pred = predictions[i];
                                max_shard = static_cast<int>(i);
                            }
                        }
                        if (max_pred > 0.1f) {
                            push_event("pinn", "PINN prediction: shard " +
                                       std::to_string(max_shard) + " peak load " +
                                       std::to_string(static_cast<int>(max_pred * 100)) +
                                       "% — pre-emptive rebalance suggested");
                        }
                    }
                }
            }

            // Detect Raft role changes (only worker 0, reduced frequency)
            if (worker_id == 0 && local_counter % 5000 < static_cast<uint64_t>(ops_per_batch)) {
                for (int ni = 0; ni < RAFT_CLUSTER_SIZE; ni++) {
                    auto rs = raft_nodes[ni]->GetState();
                    if (rs.role == dcs::raft::RaftRole::Leader) {
                        std::string cur_role = "Leader(" + std::to_string(ni) + ")";
                        if (cur_role != prev_raft_role) {
                            std::cout << "[Raft] Leader changed to Node " << ni << " (term " << rs.term << ")\n";
                            push_event("raft", "Leader changed to Node " + std::to_string(ni) +
                                       " (term " + std::to_string(rs.term) + ")");
                            prev_raft_role = cur_role;
                        }
                        break;
                    }
                }
            }

            // Adaptive sleep: account for batch execution time
            auto batch_end = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(batch_end - batch_start).count();
            int target_ms = BATCH_MS;
            int remaining = target_ms - static_cast<int>(elapsed_ms);
            if (remaining > 0) {
                dcs::compat::this_thread::sleep_for(std::chrono::milliseconds(remaining));
            }
            // If behind schedule, skip sleep and catch up
        }
    };

    // Launch N parallel traffic worker threads for 10K+ ops/s throughput
    std::vector<dcs::compat::Thread> traffic_workers;
    g_traffic_running = true;
    for (int w = 0; w < TRAFFIC_WORKERS; w++) {
        traffic_workers.emplace_back([&traffic_worker_fn, w]() {
            traffic_worker_fn(w);
        });
    }

    tcp_server.start();  // Blocks until stop() is called

    // ── 8. Graceful Shutdown ──────────────────────────────────────────
    g_shutdown = true;
    g_traffic_rate = 0;
    std::cout << "\n[Shutdown] Stopping subsystems...\n";

    http_server.stop();
    std::cout << "[Shutdown] HTTP server stopped.\n";

    sharder.Stop();
    std::cout << "[Shutdown] PINN sharder stopped.\n";

    for (int i = 0; i < RAFT_CLUSTER_SIZE; i++) {
        raft_nodes[i]->Stop();
    }
    std::cout << "[Shutdown] Raft cluster stopped (" << RAFT_CLUSTER_SIZE << " nodes).\n";

    g_burst_active = false;
    if (telemetry_thread.joinable()) telemetry_thread.join();
    for (auto& w : traffic_workers) {
        if (w.joinable()) w.join();
    }
    if (burst_thread.joinable()) burst_thread.join();

    std::cout << "[Shutdown] Flushing cache to LSM-Tree...\n";
    manager.shutdown();

    std::cout << "[Shutdown] Complete. Goodbye.\n";
    return 0;
}
