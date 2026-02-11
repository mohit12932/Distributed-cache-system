/**
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║          Distributed Cache System — "Project 42"                    ║
 * ║                                                                      ║
 * ║  A high-performance, thread-safe, Redis-compatible in-memory cache  ║
 * ║  with durable persistence and configurable write strategies.        ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 * Architecture:
 *   1. Core Engine    — O(1) LRU cache (custom doubly-linked list + hashmap)
 *   2. Concurrency    — Segmented locking (32 segments, shared_mutex per segment)
 *   3. Persistence    — File-backed storage with Write-Through & Write-Back modes
 *   4. Networking     — Multi-threaded TCP server speaking RESP protocol
 *
 * Usage:
 *   ./distributed_cache [--port PORT] [--capacity N] [--mode write-through|write-back]
 *                       [--flush-interval SECS] [--data-file PATH]
 *
 * Compatible with redis-cli:
 *   redis-cli -p 6379
 *   > SET name "Gemini"
 *   > GET name
 */

#include "include/sync/cache_manager.h"
#include "include/persistence/file_storage.h"
#include "include/network/tcp_server.h"

#include <iostream>
#include <string>
#include <csignal>
#include <cstdlib>

// ── Global shutdown flag ──────────────────────────────────────────────
static dcs::compat::Atomic<bool> g_shutdown{false};
static dcs::network::TCPServer* g_server = nullptr;

void signal_handler(int sig) {
    (void)sig;
    std::cout << "\n[Main] Caught interrupt signal — shutting down...\n";
    g_shutdown = true;
    if (g_server) g_server->stop();
}

// ── Command-line argument helpers ─────────────────────────────────────
struct ServerConfig {
    uint16_t port            = 6379;
    size_t capacity          = 65536;
    dcs::sync::WriteMode mode = dcs::sync::WriteMode::WriteBack;
    int flush_interval_sec   = 5;
    std::string data_file    = "data/cache.dat";
};

ServerConfig parse_args(int argc, char* argv[]) {
    ServerConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::atoi(argv[++i]));
        } else if ((arg == "--capacity" || arg == "-c") && i + 1 < argc) {
            cfg.capacity = static_cast<size_t>(std::atoll(argv[++i]));
        } else if ((arg == "--mode" || arg == "-m") && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "write-through" || mode == "wt") {
                cfg.mode = dcs::sync::WriteMode::WriteThrough;
            } else {
                cfg.mode = dcs::sync::WriteMode::WriteBack;
            }
        } else if ((arg == "--flush-interval" || arg == "-f") && i + 1 < argc) {
            cfg.flush_interval_sec = std::atoi(argv[++i]);
        } else if ((arg == "--data-file" || arg == "-d") && i + 1 < argc) {
            cfg.data_file = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: distributed_cache [OPTIONS]\n"
                      << "  -p, --port PORT              TCP port (default: 6379)\n"
                      << "  -c, --capacity N             Max cache entries (default: 65536)\n"
                      << "  -m, --mode MODE              write-through | write-back (default)\n"
                      << "  -f, --flush-interval SECS    Write-back flush interval (default: 5)\n"
                      << "  -d, --data-file PATH         Persistence file (default: data/cache.dat)\n"
                      << "  -h, --help                   Show this help\n";
            std::exit(0);
        }
    }
    return cfg;
}

// ── Main ──────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    std::cout << "┌─────────────────────────────────────────────┐\n";
    std::cout << "│   Distributed Cache System v1.0 (Project 42)│\n";
    std::cout << "├─────────────────────────────────────────────┤\n";
    std::cout << "│ Port:            " << cfg.port << std::string(28 - std::to_string(cfg.port).size(), ' ') << "│\n";
    std::cout << "│ Capacity:        " << cfg.capacity << std::string(28 - std::to_string(cfg.capacity).size(), ' ') << "│\n";
    std::cout << "│ Write Mode:      " << (cfg.mode == dcs::sync::WriteMode::WriteThrough ? "write-through" : "write-back   ")
              << std::string(15, ' ') << "│\n";
    std::cout << "│ Flush Interval:  " << cfg.flush_interval_sec << "s" << std::string(27 - std::to_string(cfg.flush_interval_sec).size(), ' ') << "│\n";
    std::cout << "│ Data File:       " << cfg.data_file << std::string(std::max(0, (int)(28 - cfg.data_file.size())), ' ') << "│\n";
    std::cout << "│ Segments:        32 (read-write locks)      │\n";
    std::cout << "└─────────────────────────────────────────────┘\n\n";

    // 1. Initialize persistence backend
    dcs::persistence::FileStorage storage(cfg.data_file);
    std::cout << "[Init] Loaded " << storage.disk_size() << " entries from disk.\n";

    // 2. Initialize cache manager with chosen sync strategy
    dcs::sync::CacheManager::Config cache_cfg;
    cache_cfg.cache_capacity = cfg.capacity;
    cache_cfg.write_mode     = cfg.mode;
    cache_cfg.flush_interval = std::chrono::seconds(cfg.flush_interval_sec);

    dcs::sync::CacheManager manager(cache_cfg, &storage);

    // 3. Register signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // 4. Start TCP server (blocks in accept loop)
    dcs::network::TCPServer server(cfg.port, &manager);
    g_server = &server;

    server.start();  // Blocks until stop() is called

    // 5. Graceful shutdown
    std::cout << "[Main] Flushing dirty data to disk...\n";
    manager.shutdown();
    std::cout << "[Main] Shutdown complete. Goodbye.\n";

    return 0;
}
