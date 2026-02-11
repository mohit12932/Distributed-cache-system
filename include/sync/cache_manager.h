#pragma once

#include "../cache/segmented_cache.h"
#include "../persistence/storage_backend.h"
#include "../persistence/write_back_worker.h"
#include "../compat/threading.h"

#include <string>
#include <memory>
#include <iostream>
#include <chrono>

namespace dcs {
namespace sync {

/**
 * Write mode — governs how PUT operations interact with the backend DB.
 */
enum class WriteMode {
    WriteThrough,  // Synchronous: write to cache + DB before returning OK.
    WriteBack      // Async: write to cache, return OK, flush to DB later.
};

/**
 * CacheManager — Orchestrates cache + persistence through 3 sync workflows.
 *
 * Read Path  (Cache-Aside):  cache hit? return : fetch from DB, populate cache, return.
 * Write Path (Write-Through): update cache, then synchronously write to DB.
 * Write Path (Write-Back):    update cache, return OK, background worker flushes.
 */
class CacheManager {
public:
    struct Config {
        size_t cache_capacity       = 65536;
        WriteMode write_mode        = WriteMode::WriteBack;
        std::chrono::seconds flush_interval{5};
    };

    CacheManager(Config cfg, persistence::StorageBackend* backend)
        : config_(cfg)
        , cache_(cfg.cache_capacity)
        , backend_(backend)
    {
        // Set eviction callback: on eviction of dirty data, persist it.
        cache_.set_eviction_callback(
            [this](const std::string& key, const std::string& value, bool dirty) {
                if (dirty && backend_) {
                    backend_->store(key, value);
                }
            });

        // Start write-back worker if in WriteBack mode
        if (config_.write_mode == WriteMode::WriteBack && backend_) {
            wb_worker_ = std::make_unique<persistence::WriteBackWorker>(
                backend_,
                config_.flush_interval,
                [this]() { return cache_.dirty_entries(); },
                [this](const std::string& key) { cache_.clear_dirty(key); }
            );
            wb_worker_->start();
        }
    }

    ~CacheManager() {
        shutdown();
    }

    // ── Read Path (Cache-Aside) ────────────────────────────────────

    /**
     * GET — Cache-Aside pattern.
     *   1. Check cache (fast).
     *   2. On miss, load from DB (slow), insert into cache, return.
     */
    cache::CacheResult get(const std::string& key) {
        // Step 1: Check cache
        auto result = cache_.get(key);
        if (result.hit) {
            stats_.cache_hits++;
            return result;
        }

        // Step 2: Cache miss — fetch from backend
        stats_.cache_misses++;
        if (!backend_) return cache::CacheResult::Miss();

        auto db_val = backend_->load(key);
        if (!db_val.found) {
            return cache::CacheResult::Miss();  // not in DB either
        }

        // Step 3: Populate cache (don't mark dirty — it's already in DB)
        cache_.put(key, db_val.value);
        cache_.clear_dirty(key);  // came from DB, so it's clean
        return cache::CacheResult::Hit(db_val.value);
    }

    // ── Write Path ─────────────────────────────────────────────────

    /**
     * PUT — Dispatches to WriteThrough or WriteBack based on config.
     */
    bool put(const std::string& key, const std::string& value) {
        if (config_.write_mode == WriteMode::WriteThrough) {
            return put_write_through(key, value);
        } else {
            return put_write_back(key, value);
        }
    }

    /**
     * DELETE — Remove from cache AND backend.
     */
    bool del(const std::string& key) {
        cache_.del(key);
        if (backend_) {
            backend_->remove(key);
        }
        return true;
    }

    // ── Admin ──────────────────────────────────────────────────────

    bool exists(const std::string& key) { return cache_.exists(key); }
    size_t size() const { return cache_.size(); }
    std::vector<std::string> keys() const { return cache_.keys(); }

    /** Force immediate flush of dirty data (write-back mode). */
    void flush() {
        if (wb_worker_) wb_worker_->flush();
    }

    /** Clear all entries from cache and backend (FLUSHALL). */
    void flush_all() {
        cache_.clear();   // eviction callback persists dirty data first
        // Not clearing backend on purpose for persistence semantics,
        // but to match Redis FLUSHALL we need an empty cache.
    }

    /** Graceful shutdown: flush dirty data, stop worker. */
    void shutdown() {
        if (wb_worker_) {
            wb_worker_->stop();
            wb_worker_.reset();
        }
        // Final eviction flush
        cache_.clear();
    }

    struct Stats {
        compat::Atomic<uint64_t> cache_hits{0};
        compat::Atomic<uint64_t> cache_misses{0};
        compat::Atomic<uint64_t> write_through_count{0};
        compat::Atomic<uint64_t> write_back_count{0};
    };

    const Stats& stats() const { return stats_; }
    WriteMode write_mode() const { return config_.write_mode; }

private:
    /**
     * Write-Through: Cache + DB written synchronously.
     * Returns OK only after DB confirms success.
     */
    bool put_write_through(const std::string& key, const std::string& value) {
        // Step 1: Update cache
        cache_.put(key, value);

        // Step 2: Synchronously write to DB
        if (backend_) {
            bool ok = backend_->store(key, value);
            if (!ok) {
                std::cerr << "[WriteThrough] DB write failed for key: " << key << "\n";
                return false;
            }
            cache_.clear_dirty(key);  // persisted successfully
        }

        stats_.write_through_count++;
        return true;
    }

    /**
     * Write-Back: Cache updated immediately, DB synced in background.
     * Returns OK right away — dirty flag is set on the cache entry.
     */
    bool put_write_back(const std::string& key, const std::string& value) {
        cache_.put(key, value);  // dirty flag set inside LRUCache::put
        stats_.write_back_count++;
        return true;
    }

    Config config_;
    cache::SegmentedCache cache_;
    persistence::StorageBackend* backend_;   // non-owning
    std::unique_ptr<persistence::WriteBackWorker> wb_worker_;
    Stats stats_;
};

}  // namespace sync
}  // namespace dcs
