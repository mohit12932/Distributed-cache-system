/**
 * Concurrency test suite for SegmentedCache.
 * Hammers the cache from multiple threads to verify thread safety.
 */

#include "include/cache/segmented_cache.h"
#include "include/compat/threading.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <chrono>

using Thread  = dcs::compat::Thread;
using AtomicI = dcs::compat::Atomic<int>;

#define TEST(name) \
    static void name(); \
    struct name##_reg { name##_reg() { tests.push_back({#name, name}); } } name##_inst; \
    static void name()

static std::vector<std::pair<std::string, void(*)()>> tests;

// ══════════════════════════════════════════════════════════════════════

TEST(test_concurrent_writes) {
    dcs::cache::SegmentedCache cache(4096);
    const int N_THREADS = 8;
    const int N_OPS = 10000;

    std::vector<Thread> threads;
    for (int t = 0; t < N_THREADS; ++t) {
        threads.push_back(Thread([&cache, t, N_OPS]() {
            for (int i = 0; i < N_OPS; ++i) {
                std::string key = "t" + std::to_string(t) + "_k" + std::to_string(i);
                cache.put(key, std::to_string(i));
            }
        }));
    }
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    std::cout << "    Total cached entries: " << cache.size() << "\n";
    assert(cache.size() > 0);
}

TEST(test_concurrent_reads_writes) {
    dcs::cache::SegmentedCache cache(4096);
    AtomicI read_hits(0);
    AtomicI read_misses(0);

    for (int i = 0; i < 1000; ++i) {
        cache.put("key" + std::to_string(i), "val" + std::to_string(i));
    }

    const int N_READERS = 4;
    const int N_WRITERS = 4;
    const int N_OPS = 5000;

    std::vector<Thread> threads;

    for (int t = 0; t < N_READERS; ++t) {
        threads.push_back(Thread([&cache, &read_hits, &read_misses, N_OPS]() {
            for (int i = 0; i < N_OPS; ++i) {
                auto r = cache.get("key" + std::to_string(i % 1000));
                if (r.hit) read_hits++;
                else read_misses++;
            }
        }));
    }

    for (int t = 0; t < N_WRITERS; ++t) {
        threads.push_back(Thread([&cache, t, N_OPS]() {
            for (int i = 0; i < N_OPS; ++i) {
                cache.put("key" + std::to_string(i % 1500),
                          "new_val_" + std::to_string(t) + "_" + std::to_string(i));
            }
        }));
    }

    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    std::cout << "    Reads: " << read_hits.load() << " hits, "
              << read_misses.load() << " misses\n";
    assert(read_hits.load() > 0);
}

TEST(test_concurrent_deletes) {
    dcs::cache::SegmentedCache cache(4096);

    for (int i = 0; i < 2000; ++i) {
        cache.put("d" + std::to_string(i), "v" + std::to_string(i));
    }

    std::vector<Thread> threads;

    for (int t = 0; t < 4; ++t) {
        threads.push_back(Thread([&cache, t]() {
            for (int i = t * 500; i < (t + 1) * 500; ++i) {
                cache.del("d" + std::to_string(i));
            }
        }));
    }

    for (int t = 0; t < 4; ++t) {
        (void)t;
        threads.push_back(Thread([&cache]() {
            for (int i = 0; i < 2000; ++i) {
                cache.get("d" + std::to_string(i));
            }
        }));
    }

    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
    std::cout << "    Remaining entries after concurrent deletes: " << cache.size() << "\n";
}

TEST(test_segment_isolation) {
    dcs::cache::SegmentedCache cache(4096);
    const int N = 5000;

    Thread writer([&cache, N]() {
        for (int i = 0; i < N; ++i) {
            cache.put("w_" + std::to_string(i), std::to_string(i * 10));
        }
    });

    Thread reader([&cache, N]() {
        for (int i = 0; i < N; ++i) {
            auto r = cache.get("w_" + std::to_string(i));
            if (r.hit) {
                assert(r.value == std::to_string(i * 10));
            }
        }
    });

    writer.join();
    reader.join();
}

TEST(test_stress_mixed_operations) {
    dcs::cache::SegmentedCache cache(2048);
    const int N_THREADS = 16;
    const int N_OPS = 5000;

    auto start = std::chrono::steady_clock::now();

    std::vector<Thread> threads;
    for (int t = 0; t < N_THREADS; ++t) {
        threads.push_back(Thread([&cache, t, N_OPS]() {
            for (int i = 0; i < N_OPS; ++i) {
                std::string key = "stress_" + std::to_string((t * N_OPS + i) % 3000);
                switch (i % 3) {
                    case 0: cache.put(key, "v" + std::to_string(i)); break;
                    case 1: cache.get(key); break;
                    case 2: cache.del(key); break;
                }
            }
        }));
    }

    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();

    auto elapsed = std::chrono::steady_clock::now() - start;
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    long long total_ops = (long long)N_THREADS * N_OPS;

    std::cout << "    " << total_ops << " ops in " << ms << " ms ("
              << (ms > 0 ? total_ops * 1000 / ms : 0) << " ops/sec)\n";
}

// ══════════════════════════════════════════════════════════════════════

int main() {
    int passed = 0, failed = 0;
    std::cout << "=== Concurrency Tests ===\n\n";

    for (size_t i = 0; i < tests.size(); ++i) {
        try {
            tests[i].second();
            std::cout << "  [PASS] " << tests[i].first << "\n\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "  [FAIL] " << tests[i].first << ": " << e.what() << "\n\n";
            ++failed;
        } catch (...) {
            std::cout << "  [FAIL] " << tests[i].first << ": assertion failed\n\n";
            ++failed;
        }
    }

    std::cout << "Results: " << passed << " passed, " << failed << " failed.\n";
    return failed > 0 ? 1 : 0;
}
