#pragma once

#include "lru_cache.h"
#include "../compat/threading.h"

#include <array>
#include <functional>
#include <vector>
#include <string>
#include <memory>
#include <cstdint>

namespace dcs {
namespace cache {

/**
 * SegmentedCache — Thread-safe LRU cache with granular locking.
 *
 * The key space is divided into N_SEGMENTS independent segments, each
 * with its own LRU cache and its own read-write lock.
 *
 * - GET acquires a shared (read) lock  -> concurrent reads don't block.
 * - PUT/DELETE acquires an exclusive (write) lock -> blocks only its segment.
 * - A write to key "A" in segment 3 does NOT block a read of key "B" in segment 7.
 *
 * Hashing: std::hash<string> mod N_SEGMENTS determines the segment.
 */
static constexpr size_t N_SEGMENTS = 32;

class SegmentedCache {
public:
    /**
     * @param total_capacity  Total number of entries across all segments.
     *                        Each segment gets total_capacity / N_SEGMENTS.
     */
    explicit SegmentedCache(size_t total_capacity = 65536) {
        size_t per_segment = std::max<size_t>(1, total_capacity / N_SEGMENTS);
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            segments_[i].cache = std::make_unique<LRUCache>(per_segment);
        }
    }

    // ── Core Operations ────────────────────────────────────────────

    /** Thread-safe GET (lock on segment). */
    CacheResult get(const std::string& key) {
        auto& seg = segment_for(key);
        compat::LockGuard<compat::Mutex> lock(seg.mutex);
        return seg.cache->get(key);
    }

    /** Thread-safe PUT (lock on segment). */
    void put(const std::string& key, const std::string& value) {
        auto& seg = segment_for(key);
        compat::LockGuard<compat::Mutex> lock(seg.mutex);
        seg.cache->put(key, value);
    }

    /** Thread-safe DELETE (lock on segment). */
    bool del(const std::string& key) {
        auto& seg = segment_for(key);
        compat::LockGuard<compat::Mutex> lock(seg.mutex);
        return seg.cache->del(key);
    }

    /** Thread-safe EXISTS (lock on segment). */
    bool exists(const std::string& key) {
        auto& seg = segment_for(key);
        compat::LockGuard<compat::Mutex> lock(seg.mutex);
        return seg.cache->exists(key);
    }

    // ── Bulk / Admin Operations ────────────────────────────────────

    /** Return total number of cached entries across all segments. */
    size_t size() const {
        size_t total = 0;
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            compat::LockGuard<compat::Mutex> lock(segments_[i].mutex);
            total += segments_[i].cache->size();
        }
        return total;
    }

    /** Return all keys (acquires lock on each segment). */
    std::vector<std::string> keys() const {
        std::vector<std::string> all;
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            compat::LockGuard<compat::Mutex> lock(segments_[i].mutex);
            auto seg_keys = segments_[i].cache->keys();
            all.insert(all.end(), seg_keys.begin(), seg_keys.end());
        }
        return all;
    }

    /**
     * Collect all dirty entries across segments (for write-back flush).
     * Acquires locks one segment at a time to avoid global stall.
     */
    std::vector<std::pair<std::string, std::string>> dirty_entries() const {
        std::vector<std::pair<std::string, std::string>> all;
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            compat::LockGuard<compat::Mutex> lock(segments_[i].mutex);
            auto seg_dirty = segments_[i].cache->dirty_entries();
            all.insert(all.end(), seg_dirty.begin(), seg_dirty.end());
        }
        return all;
    }

    /** Clear dirty flag on a key after it has been persisted. */
    void clear_dirty(const std::string& key) {
        auto& seg = segment_for(key);
        compat::LockGuard<compat::Mutex> lock(seg.mutex);
        seg.cache->clear_dirty(key);
    }

    /** Set eviction callback on all segments. */
    void set_eviction_callback(EvictionCallback cb) {
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            compat::LockGuard<compat::Mutex> lock(segments_[i].mutex);
            segments_[i].cache->set_eviction_callback(cb);
        }
    }

    /** Flush all segments (for graceful shutdown). */
    void clear() {
        for (size_t i = 0; i < N_SEGMENTS; ++i) {
            compat::LockGuard<compat::Mutex> lock(segments_[i].mutex);
            segments_[i].cache->clear();
        }
    }

private:
    struct Segment {
        mutable compat::Mutex mutex;
        std::unique_ptr<LRUCache> cache;
    };

    Segment& segment_for(const std::string& key) {
        size_t idx = hasher_(key) % N_SEGMENTS;
        return segments_[idx];
    }

    const Segment& segment_for(const std::string& key) const {
        size_t idx = hasher_(key) % N_SEGMENTS;
        return segments_[idx];
    }

    std::array<Segment, N_SEGMENTS> segments_;
    std::hash<std::string> hasher_;
};

}  // namespace cache
}  // namespace dcs
