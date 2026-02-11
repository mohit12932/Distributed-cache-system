#pragma once

#include "node.h"

#include <string>
#include <unordered_map>
#include <functional>
#include <vector>
#include <utility>

namespace dcs {
namespace cache {

/**
 * Result of a cache operation.
 */
struct CacheResult {
    bool hit;
    std::string value;

    static CacheResult Hit(const std::string& v) { return {true, v}; }
    static CacheResult Miss() { return {false, ""}; }
};

/**
 * Eviction callback: called when a node is evicted from the cache.
 * Signature: void(const std::string& key, const std::string& value, bool dirty)
 */
using EvictionCallback = std::function<void(const std::string&, const std::string&, bool)>;

/**
 * LRU Cache — O(1) GET, PUT, DELETE.
 *
 * Uses a custom doubly linked list + unordered_map.
 * NOT thread-safe by itself — concurrency is handled by SegmentedCache.
 */
class LRUCache {
public:
    explicit LRUCache(size_t capacity = 1024)
        : capacity_(capacity) {}

    ~LRUCache() = default;

    // Non-copyable
    LRUCache(const LRUCache&) = delete;
    LRUCache& operator=(const LRUCache&) = delete;

    /**
     * GET — Retrieve a value by key.
     * On hit: moves the node to MRU and returns the value.
     * On miss: returns CacheResult::Miss().
     */
    CacheResult get(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return CacheResult::Miss();
        }

        Node* node = it->second;
        node->last_access = std::chrono::steady_clock::now();
        list_.move_to_front(node);
        return CacheResult::Hit(node->value);
    }

    /**
     * PUT — Insert or update a key-value pair.
     * If key exists: updates value, marks dirty, moves to MRU.
     * If key is new and cache is full: evicts LRU entry first.
     */
    void put(const std::string& key, const std::string& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // Key exists — update in place
            Node* node = it->second;
            node->value = value;
            node->dirty = true;
            node->last_access = std::chrono::steady_clock::now();
            list_.move_to_front(node);
            return;
        }

        // Evict if at capacity
        while (map_.size() >= capacity_) {
            evict_lru();
        }

        // Insert new node at MRU position
        Node* node = new Node(key, value);
        node->dirty = true;
        list_.push_front(node);
        map_[key] = node;
    }

    /**
     * DELETE — Remove a key from the cache.
     * Returns true if the key existed and was removed.
     */
    bool del(const std::string& key) {
        auto it = map_.find(key);
        if (it == map_.end()) return false;

        Node* node = it->second;
        list_.detach(node);
        map_.erase(it);

        if (eviction_cb_) {
            eviction_cb_(node->key, node->value, node->dirty);
        }

        delete node;
        return true;
    }

    /** Check if a key exists without promoting it. */
    bool exists(const std::string& key) const {
        return map_.find(key) != map_.end();
    }

    /** Return all keys currently in the cache. */
    std::vector<std::string> keys() const {
        std::vector<std::string> result;
        result.reserve(map_.size());
        for (auto it = map_.begin(); it != map_.end(); ++it) {
            result.push_back(it->first);
        }
        return result;
    }

    /** Collect all dirty keys (for write-back flush). */
    std::vector<std::pair<std::string, std::string>> dirty_entries() const {
        std::vector<std::pair<std::string, std::string>> result;
        Node* curr = list_.head_sentinel()->next;
        Node* tail = list_.tail_sentinel();
        while (curr != tail) {
            if (curr->dirty) {
                result.emplace_back(curr->key, curr->value);
            }
            curr = curr->next;
        }
        return result;
    }

    /** Clear dirty flag for a key (after successful persistence). */
    void clear_dirty(const std::string& key) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second->dirty = false;
        }
    }

    /** Set the eviction callback. */
    void set_eviction_callback(EvictionCallback cb) {
        eviction_cb_ = std::move(cb);
    }

    size_t size() const { return map_.size(); }
    size_t capacity() const { return capacity_; }
    bool empty() const { return map_.empty(); }

    /** Flush entire cache (for shutdown). */
    void clear() {
        // Evict all entries through callback
        while (!map_.empty()) {
            evict_lru();
        }
    }

private:
    void evict_lru() {
        Node* lru = list_.pop_back();
        if (!lru) return;

        if (eviction_cb_) {
            eviction_cb_(lru->key, lru->value, lru->dirty);
        }

        map_.erase(lru->key);
        delete lru;
    }

    size_t capacity_;
    DoublyLinkedList list_;
    std::unordered_map<std::string, Node*> map_;
    EvictionCallback eviction_cb_;
};

}  // namespace cache
}  // namespace dcs
