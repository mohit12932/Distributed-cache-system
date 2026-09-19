#include "include/sync/cache_manager.h"
#include "include/storage/lsm_engine.h"
#include "include/raft/raft_node.h"
#include "include/raft/tcp_transport.h"
#include "include/ml/predictive_sharder.h"
#include "include/network/tcp_server.h"
#include "include/network/http_server.h"

#include "include/network/http_server.h"

#include <iostream>
#include <sstream>
#include <string>
#include <csignal>
#include <cstdlib>
#include <chrono>

// Lightweight fallback logger for environments without spdlog
namespace spdlog {
    template<typename... Args>
    void info(const char* msg, Args... args) {
        std::cout << "[INFO] " << msg << "\n";
    }
    template<typename... Args>
    void warn(const char* msg, Args... args) {
        std::cout << "[WARN] " << msg << "\n";
    }
    template<typename... Args>
    void error(const char* msg, Args... args) {
        std::cerr << "[ERROR] " << msg << "\n";
    }
    inline void set_pattern(const char*) {}
}

static dcs::compat::Atomic<bool> g_shutdown{false};
static dcs::network::TCPServer*  g_tcp_server  = nullptr;
static dcs::network::HTTPServer* g_http_server = nullptr;

void signal_handler(int sig) {
    (void)sig;
    spdlog::info("Caught interrupt signal — shutting down...");
    g_shutdown = true;
    if (g_tcp_server)  g_tcp_server->stop();
    if (g_http_server) g_http_server->stop();
}

struct ServerConfig {
    uint16_t    port             = 6379;
    uint16_t    http_port        = 8080;
    size_t      capacity         = 1048576;
    dcs::sync::WriteMode mode    = dcs::sync::WriteMode::WriteBack;
    int         flush_interval   = 5;
    std::string data_dir         = "data";
    int         node_id          = 0;
    int         cluster_size     = 5;
    std::string auth_password    = "";
};

static uint16_t env_port(const char* name, uint16_t fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    long parsed = std::strtol(value, nullptr, 10);
    if (parsed < 1 || parsed > 65535) return fallback;
    return static_cast<uint16_t>(parsed);
}

static std::string env_str(const char* name, const std::string& fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::string(value);
}

ServerConfig load_config(int argc, char* argv[]) {
    ServerConfig cfg;
    cfg.http_port = env_port("PORT", cfg.http_port);
    cfg.auth_password = env_str("CACHE_AUTH_PASS", cfg.auth_password);

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
        else if (arg == "--auth" && i + 1 < argc)
            cfg.auth_password = argv[++i];
    }
    return cfg;
}

int main(int argc, char* argv[]) {
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] %v");
    spdlog::info("Starting Distributed Cache System v2.0 (Production Mode)");

    auto cfg = load_config(argc, argv);
    std::string mode_str = (cfg.mode == dcs::sync::WriteMode::WriteThrough)
                           ? "write-through" : "write-back";

    spdlog::info("Configuration:");
    spdlog::info("  TCP Port: {}", cfg.port);
    spdlog::info("  HTTP Port (Metrics): {}", cfg.http_port);
    spdlog::info("  Capacity: {}", cfg.capacity);
    spdlog::info("  Write Mode: {}", mode_str);
    spdlog::info("  Auth: {}", (cfg.auth_password.empty() ? "Disabled" : "Enabled"));

    // 1. Storage Engine
    spdlog::info("Initializing LSM-Tree Engine...");
    dcs::storage::LSMEngine lsm_storage(cfg.data_dir + "/lsm");
    spdlog::info("LSM-Tree Engine ready.");

    // 2. Cache Manager
    dcs::sync::CacheManager::Config cache_cfg;
    cache_cfg.cache_capacity = cfg.capacity;
    cache_cfg.write_mode     = cfg.mode;
    cache_cfg.flush_interval = std::chrono::seconds(cfg.flush_interval);

    dcs::sync::CacheManager manager(cache_cfg, &lsm_storage);
    spdlog::info("Cache Manager initialized.");

    // 3. Consensus
    spdlog::info("Initializing Raft node...");
    dcs::raft::TCPRaftTransport raft_transport;
    dcs::raft::RaftNode raft_node(cfg.node_id, cfg.cluster_size, cfg.data_dir + "/raft");
    raft_node.SetTransport(&raft_transport);
    raft_node.Start();

    // 4. ML Engine
    spdlog::info("Starting PINN load predictor...");
    dcs::ml::PINNConfig pinn_cfg;
    pinn_cfg.hidden_size   = 64;
    pinn_cfg.num_layers    = 4;
    pinn_cfg.learning_rate = 1e-3f;
    pinn_cfg.lambda_pde    = 0.1f;
    pinn_cfg.nu            = 0.01f;
    dcs::ml::PredictiveSharder sharder(32, pinn_cfg);
    sharder.Start();

    // Start background telemetry poller
    dcs::compat::Thread telemetry_thread([&manager, &sharder]() {
        while (!g_shutdown) {
            auto segment_sizes = manager.segment_sizes();
            for (size_t i = 0; i < segment_sizes.size(); ++i) {
                // Approximate load: segment size / (capacity per segment)
                float load = static_cast<float>(segment_sizes[i]) / (1048576.0f / 32.0f);
                if (load > 1.0f) load = 1.0f;
                // Dummy values for hit rate and latency (could be expanded)
                sharder.RecordTelemetry(i, load, 0.8f, 1.5f);
            }
            dcs::compat::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 5. Metrics Server
    spdlog::info("Starting Metrics server on port {}", cfg.http_port);
    dcs::network::HTTPServer http_server(cfg.http_port, "web"); // Passing "web" just to satisfy constructor, UI is dead
    g_http_server = &http_server;

    http_server.setMetricsCallback([&]() -> std::string {
        auto& cache_stats = manager.stats();
        auto& lsm_stats   = lsm_storage.Stats();
        
        std::ostringstream prom;
        prom << "# HELP dcs_cache_hits Total cache hits\n";
        prom << "# TYPE dcs_cache_hits counter\n";
        prom << "dcs_cache_hits " << cache_stats.cache_hits.load() << "\n";
        
        prom << "# HELP dcs_cache_misses Total cache misses\n";
        prom << "# TYPE dcs_cache_misses counter\n";
        prom << "dcs_cache_misses " << cache_stats.cache_misses.load() << "\n";
        
        prom << "# HELP dcs_cache_size Current cache entries\n";
        prom << "# TYPE dcs_cache_size gauge\n";
        prom << "dcs_cache_size " << manager.size() << "\n";

        prom << "# HELP dcs_lsm_compactions Total LSM compactions\n";
        prom << "# TYPE dcs_lsm_compactions counter\n";
        prom << "dcs_lsm_compactions " << lsm_stats.compactions_done.load() << "\n";

        auto sharder_stats = sharder.GetStats();
        prom << "# HELP dcs_ml_pinn_loss Current total loss of the PINN load predictor\n";
        prom << "# TYPE dcs_ml_pinn_loss gauge\n";
        prom << "dcs_ml_pinn_loss " << sharder_stats.total_loss << "\n";
        
        prom << "# HELP dcs_ml_telemetry_count Number of telemetry samples collected\n";
        prom << "# TYPE dcs_ml_telemetry_count counter\n";
        prom << "dcs_ml_telemetry_count " << sharder_stats.telemetry_count << "\n";

        auto recs = sharder.GetRecommendations(0.7f);
        prom << "# HELP dcs_ml_migrations Recommended shard migrations\n";
        prom << "# TYPE dcs_ml_migrations gauge\n";
        prom << "dcs_ml_migrations " << recs.size() << "\n";

        return prom.str();
    });
    http_server.start();

    // 6. TCP Server
    spdlog::info("Starting TCP server on port {}", cfg.port);
    dcs::network::TCPServer tcp_server(cfg.port, &manager);
    tcp_server.set_auth_password(cfg.auth_password);
    g_tcp_server = &tcp_server;
    
    if (!tcp_server.start()) {
        spdlog::error("Failed to start TCP server.");
        return 1;
    }

    if (telemetry_thread.joinable()) {
        telemetry_thread.join();
    }

    spdlog::info("Server stopped gracefully.");
    return 0;
}
