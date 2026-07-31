# 🚀 AI-Adaptive Distributed Cache System

[![Build Status](https://img.shields.io/badge/build-passing-brightgreen)](https://github.com/mohit12932/Distributed-cache-system/actions)
[![Tests](https://img.shields.io/badge/tests-58%20passed-brightgreen)]()
[![C++](https://img.shields.io/badge/C++-17-blue.svg)](https://isocpp.org/)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Redis Compatible](https://img.shields.io/badge/Redis-Compatible-red.svg)]()

> A high-performance, thread-safe, Redis-compatible in-memory cache with durable persistence, AI-powered predictive sharding, and comprehensive chaos engineering tests.

## 🌟 Key Features

| Feature | Description |
|---------|-------------|
| **O(1) LRU Cache** | Custom doubly-linked list + hashmap for constant-time operations |
| **Segmented Locking** | 32 independent segments for high-concurrency (~232K ops/sec) |
| **Redis Protocol** | Full RESP2 support - works with `redis-cli` and any Redis client |
| **Dual Persistence** | Write-Through (sync) or Write-Back (async) strategies |
| **AI Predictive Sharding** | Physics-Informed Neural Network for traffic prediction |
| **Raft Consensus** | Distributed coordination with leader election |
| **Chaos Testing** | Fault injection framework for reliability testing |

## 🏗️ Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Client Layer                                  │
│                   (redis-cli / Any Redis Client)                    │
├─────────────────────────────────────────────────────────────────────┤
│                                                                      │
│  ┌──────────────────┐    ┌──────────────────┐    ┌───────────────┐ │
│  │   TCP Server     │    │  RESP Parser     │    │Client Handler │ │
│  │ (Multi-threaded) │───▶│  (RESP2 Proto)   │───▶│(SET/GET/DEL)  │ │
│  └──────────────────┘    └──────────────────┘    └───────────────┘ │
│                                                          │          │
├──────────────────────────────────────────────────────────┼──────────┤
│                                                          ▼          │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                     Cache Manager                             │  │
│  │  ┌─────────────────┐  ┌─────────────────┐  ┌──────────────┐  │  │
│  │  │  Cache-Aside    │  │ Write-Through   │  │  Write-Back  │  │  │
│  │  │  (Read Path)    │  │ (Sync Persist)  │  │(Async Flush) │  │  │
│  │  └─────────────────┘  └─────────────────┘  └──────────────┘  │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                    │                                │
├────────────────────────────────────┼────────────────────────────────┤
│                                    ▼                                │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                   Segmented Cache (32 Segments)               │  │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐       ┌─────────┐       │  │
│  │  │Segment 0│ │Segment 1│ │Segment 2│  ...  │Segment31│       │  │
│  │  │ LRU     │ │ LRU     │ │ LRU     │       │ LRU     │       │  │
│  │  │ Cache   │ │ Cache   │ │ Cache   │       │ Cache   │       │  │
│  │  └─────────┘ └─────────┘ └─────────┘       └─────────┘       │  │
│  └──────────────────────────────────────────────────────────────┘  │
│                                    │                                │
├────────────────────────────────────┼────────────────────────────────┤
│                                    ▼                                │
│  ┌──────────────────────────────────────────────────────────────┐  │
│  │                     Persistence Layer                         │  │
│  │  ┌─────────────────┐  ┌─────────────────────────────────┐    │  │
│  │  │  File Storage   │  │  Write-Back Worker (Background) │    │  │
│  │  │  (KEY\tVAL\n)   │  │  (Periodic Flush Thread)        │    │  │
│  │  └─────────────────┘  └─────────────────────────────────┘    │  │
│  └──────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```
## 📸 Screenshots

<img width="1428" height="600" alt="Screenshot 2026-02-16 003806" src="https://github.com/user-attachments/assets/5638764f-abbb-49ec-91f4-3d6ade550875" />
<img width="1426" height="539" alt="image" src="https://github.com/user-attachments/assets/5608bb65-4e24-4da1-9d3c-b5113051659d" />



*The main dashboard view showing user analytics.*



## 📊 Performance

| Metric | Value |
|--------|-------|
| **Throughput** | ~232,000 ops/sec |
| **Read Latency** | < 1ms (p50) |
| **Concurrent Writers** | 16 threads tested |
| **Cache Hit Rate** | 100% (in-memory) |
| **Segments** | 32 (configurable) |

## 🚀 Quick Start

### Prerequisites

- C++17 compatible compiler (GCC 6.3+, MSVC 2017+, Clang 5+)
- CMake 3.16+ (optional)
- Windows: Winsock2 | Linux/Mac: POSIX sockets

### Build

**Windows (MinGW):**
```powershell
cd "Distributed cache system"
mkdir build
g++ -std=c++17 -O2 -I. -o build/distributed_cache.exe src/main.cpp -lws2_32
```

**Linux/Mac:**
```bash
g++ -std=c++17 -O2 -I. -o build/distributed_cache src/main.cpp -pthread
```

**CMake (Cross-platform):**
```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

### Run Server

```bash
./build/distributed_cache --port 6379 --capacity 65536 --mode write-back

# Options:
#   -p, --port PORT              TCP port (default: 6379)
#   -c, --capacity N             Max cache entries (default: 65536)
#   -m, --mode MODE              write-through | write-back (default)
#   -f, --flush-interval SECS    Write-back flush interval (default: 5)
#   -d, --data-file PATH         Persistence file (default: data/cache.dat)
```

### Connect with redis-cli

```bash
redis-cli -p 6379

127.0.0.1:6379> SET user:1 "Alice"
OK
127.0.0.1:6379> GET user:1
"Alice"
127.0.0.1:6379> SET counter 100
OK
127.0.0.1:6379> EXISTS counter
(integer) 1
127.0.0.1:6379> DEL counter
(integer) 1
127.0.0.1:6379> KEYS *
1) "user:1"
127.0.0.1:6379> INFO
# Server
distributed_cache_version:1.0.0
write_mode:write-back

# Stats
cache_hits:5
cache_misses:0
```

## 🌍 Deployment Split

Use Vercel for the static dashboard and Render for the backend service.

### Vercel frontend

The frontend can be deployed as a static site from this repository. The dashboard reads its backend from either the `backend` query string, `localStorage.dcsBackendUrl`, or the `window.__API_BASE__` override.

Example:

```text
https://your-vercel-app.vercel.app/?backend=https://distributed-cache-system-mfd7.onrender.com
```

### Render backend

Render should run the backend process with its provided `PORT` environment variable. The HTTP dashboard/API listens on that port, while the TCP RESP listener remains available inside the process.

Local build and run still work the same way:

```powershell
cmake -S . -B build
cmake --build build
build\distributed_cache.exe --port 6379 --mode write-back
```

## 🧪 Testing

### Run All Tests

```powershell
# Windows
.\demo\run_all_tests.ps1

# Linux/Mac
./demo/run_all_tests.sh
```

### Individual Test Suites

```bash
# 1. LRU Cache Core (11 tests)
./build/test_lru_cache

# 2. Concurrency Stress (5 tests)
./build/test_concurrency

# 3. RESP Protocol & Handler (16 tests)
./build/test_resp_parser

# 4. Live Server Integration (26 tests)
./build/distributed_cache --port 6399 &
./build/test_live_server
```

### Test Results

```
=== Test Summary ===
┌─────────────────────────┬───────┬────────┐
│ Suite                   │ Tests │ Result │
├─────────────────────────┼───────┼────────┤
│ LRU Cache Core          │ 11    │ PASS   │
│ Concurrency Stress      │ 5     │ PASS   │
│ RESP Parser & Handler   │ 16    │ PASS   │
│ Live Server Integration │ 26    │ PASS   │
├─────────────────────────┼───────┼────────┤
│ TOTAL                   │ 58    │ PASS   │
└─────────────────────────┴───────┴────────┘
```

## 📁 Project Structure

```
Distributed-cache-system/
├── src/
│   ├── main.cpp                 # Server entry point
│   └── tests/
│       ├── test_lru_cache.cpp   # LRU cache unit tests
│       ├── test_concurrency.cpp # Thread-safety tests
│       └── test_resp_parser.cpp # Protocol tests
├── include/
│   ├── cache/
│   │   ├── lru_cache.h          # O(1) LRU implementation
│   │   ├── node.h               # Doubly-linked list node
│   │   └── segmented_cache.h    # 32-segment concurrent cache
│   ├── network/
│   │   ├── tcp_server.h         # Multi-threaded TCP server
│   │   ├── client_handler.h     # Command dispatcher
│   │   └── resp_parser.h        # RESP2 protocol codec
│   ├── persistence/
│   │   ├── storage_backend.h    # Abstract storage interface
│   │   ├── file_storage.h       # File-based persistence
│   │   └── write_back_worker.h  # Background flush thread
│   ├── sync/
│   │   └── cache_manager.h      # Cache-aside + write strategies
│   └── compat/
│       └── threading.h          # Cross-platform threading
├── tests/
│   └── test_live_server.cpp     # Integration tests
├── demo/
│   ├── run_all_tests.ps1        # Windows test runner
│   ├── run_all_tests.sh         # Linux/Mac test runner
│   └── demo_showcase.ps1        # HR demo script
├── ai_kv_store/                  # Advanced AI features
│   ├── include/
│   │   ├── ml/                  # PINN model
│   │   ├── raft/                # Raft consensus
│   │   └── storage/             # LSM-Tree
│   └── docs/
│       └── BLUEPRINT.md         # Architecture design
├── .github/
│   └── workflows/
│       └── ci.yml               # GitHub Actions CI/CD
├── README.md
├── SHOWCASE.md                  # HR presentation guide
├── LICENSE
└── .gitignore
```

## 🎯 Supported Commands

| Command | Syntax | Description |
|---------|--------|-------------|
| `SET` | `SET key value` | Store a key-value pair |
| `GET` | `GET key` | Retrieve value by key |
| `DEL` | `DEL key [key ...]` | Delete one or more keys |
| `EXISTS` | `EXISTS key` | Check if key exists |
| `KEYS` | `KEYS *` | List all keys |
| `DBSIZE` | `DBSIZE` | Return total key count |
| `FLUSHALL` | `FLUSHALL` | Delete all keys |
| `PING` | `PING [message]` | Health check |
| `INFO` | `INFO` | Server statistics |
| `QUIT` | `QUIT` | Close connection |

## 🔬 Technical Highlights

### 1. O(1) LRU Eviction

```cpp
// Custom doubly-linked list ensures O(1) move-to-front
void LRUCache::get(const std::string& key) {
    Node* node = map_[key];
    node->last_access = now();
    list_.move_to_front(node);  // O(1) pointer manipulation
    return node->value;
}
```

### 2. Segmented Locking

```cpp
// Hash-based segment selection avoids global lock
Segment& segment_for(const std::string& key) {
    size_t idx = std::hash<std::string>{}(key) % 32;
    return segments_[idx];  // Each segment has its own mutex
}
```

### 3. Write-Back Persistence

```cpp
// Background worker periodically flushes dirty entries
void WriteBackWorker::run_loop() {
    while (running_) {
        cv_.wait_for(lock, interval_);
        auto dirty = collector_();
        backend_->batch_store(dirty);
    }
}
```

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/amazing-feature`)
3. Commit your changes (`git commit -m 'Add amazing feature'`)
4. Push to the branch (`git push origin feature/amazing-feature`)
5. Open a Pull Request

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 👤 Author

**Mohit**
- GitHub: [@mohit12932](https://github.com/mohit12932)

## 🙏 Acknowledgments

- Redis for protocol inspiration
- The C++ community for best practices
- All open-source contributors

---

⭐ **Star this repo if you find it useful!**
