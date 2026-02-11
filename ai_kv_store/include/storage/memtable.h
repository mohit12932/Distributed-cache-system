// ai_kv_store/include/storage/memtable.h
// ────────────────────────────────────────────────────────────────
// Lock-free skip list MemTable for the LSM-Tree write path.
// Provides O(log n) insert/lookup with concurrent readers.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"

namespace ai_kv {
namespace storage {

// ──────────────────────────────────────────────────────────────
//  InternalKey: user_key + sequence_number + type
// ──────────────────────────────────────────────────────────────

enum class ValueType : uint8_t {
    kValue    = 0x01,
    kDeletion = 0x02,
};

struct InternalKey {
    std::string user_key;
    uint64_t    sequence;     // Monotonically increasing per-write
    ValueType   type;

    // Ordering: user_key ASC, then sequence DESC (newest first)
    bool operator<(const InternalKey& rhs) const {
        int cmp = user_key.compare(rhs.user_key);
        if (cmp != 0) return cmp < 0;
        return sequence > rhs.sequence;  // Descending — newest wins
    }

    bool operator==(const InternalKey& rhs) const {
        return user_key == rhs.user_key && sequence == rhs.sequence;
    }
};

// ──────────────────────────────────────────────────────────────
//  SkipList Node
// ──────────────────────────────────────────────────────────────

static constexpr int kMaxHeight = 20;

struct SkipNode {
    InternalKey key;
    std::string value;
    int         height;
    // Variable-length array of forward pointers (atomic for lock-free reads)
    std::atomic<SkipNode*> forward[1];  // Actual size = height

    static SkipNode* Create(const InternalKey& k, const std::string& v, int h) {
        // Allocate node + extra forward pointers
        size_t sz = sizeof(SkipNode) + sizeof(std::atomic<SkipNode*>) * (h - 1);
        void* mem = ::operator new(sz);
        SkipNode* node = new (mem) SkipNode();
        node->key    = k;
        node->value  = v;
        node->height = h;
        for (int i = 0; i < h; ++i) {
            node->forward[i].store(nullptr, std::memory_order_relaxed);
        }
        return node;
    }

    // No copy/move — allocated in-place
    SkipNode(const SkipNode&) = delete;
    SkipNode& operator=(const SkipNode&) = delete;

private:
    SkipNode() = default;
};

// ──────────────────────────────────────────────────────────────
//  SkipList-based MemTable
// ──────────────────────────────────────────────────────────────

class MemTable {
public:
    explicit MemTable(size_t max_size_bytes = 4 * 1024 * 1024)
        : max_size_bytes_(max_size_bytes),
          approximate_size_(0),
          entry_count_(0),
          max_height_(1),
          sequence_(0) {
        head_ = SkipNode::Create(InternalKey{"", 0, ValueType::kValue}, "", kMaxHeight);
    }

    ~MemTable() {
        SkipNode* node = head_;
        while (node) {
            SkipNode* next = node->forward[0].load(std::memory_order_relaxed);
            ::operator delete(node);
            node = next;
        }
    }

    // ── Write operations (serialized via external mutex or single-writer) ──

    // Insert a key-value pair. Returns the assigned sequence number.
    uint64_t Put(absl::string_view user_key, absl::string_view value) {
        uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
        InternalKey ikey{std::string(user_key), seq, ValueType::kValue};
        Insert(ikey, std::string(value));
        return seq;
    }

    // Insert a deletion marker (tombstone).
    uint64_t Delete(absl::string_view user_key) {
        uint64_t seq = sequence_.fetch_add(1, std::memory_order_relaxed);
        InternalKey ikey{std::string(user_key), seq, ValueType::kDeletion};
        Insert(ikey, "");
        return seq;
    }

    // ── Read operations (lock-free, safe for concurrent readers) ──

    struct LookupResult {
        bool        found;
        bool        is_deletion;  // True if key was explicitly deleted
        std::string value;

        static LookupResult Hit(const std::string& v) { return {true, false, v}; }
        static LookupResult Deleted()                  { return {true, true, ""}; }
        static LookupResult Miss()                     { return {false, false, ""}; }
    };

    // Find the latest version of user_key.
    LookupResult Get(absl::string_view user_key) const {
        SkipNode* node = FindGreaterOrEqual(
            InternalKey{std::string(user_key), UINT64_MAX, ValueType::kValue});

        if (node && node->key.user_key == user_key) {
            if (node->key.type == ValueType::kDeletion) {
                return LookupResult::Deleted();
            }
            return LookupResult::Hit(node->value);
        }
        return LookupResult::Miss();
    }

    // ── Iteration (for flush to SSTable) ──

    // Callback: (InternalKey, value) → void.  Iterated in sorted order.
    using IterCallback = std::function<void(const InternalKey&, const std::string&)>;

    void ForEach(IterCallback cb) const {
        SkipNode* node = head_->forward[0].load(std::memory_order_acquire);
        while (node) {
            cb(node->key, node->value);
            node = node->forward[0].load(std::memory_order_acquire);
        }
    }

    // ── Capacity ──

    bool ShouldFlush() const {
        return approximate_size_.load(std::memory_order_relaxed) >= max_size_bytes_;
    }

    size_t ApproximateSize() const {
        return approximate_size_.load(std::memory_order_relaxed);
    }

    size_t EntryCount() const {
        return entry_count_.load(std::memory_order_relaxed);
    }

    uint64_t MaxSequence() const {
        return sequence_.load(std::memory_order_relaxed);
    }

    // ── Range scan: collect entries in [start, end) ──

    std::vector<std::pair<InternalKey, std::string>>
    Scan(absl::string_view start_key, absl::string_view end_key, uint32_t limit) const {
        std::vector<std::pair<InternalKey, std::string>> results;
        InternalKey search_key{std::string(start_key), UINT64_MAX, ValueType::kValue};
        SkipNode* node = FindGreaterOrEqual(search_key);

        while (node && results.size() < limit) {
            if (!end_key.empty() && node->key.user_key >= end_key) break;
            results.emplace_back(node->key, node->value);
            node = node->forward[0].load(std::memory_order_acquire);
        }
        return results;
    }

private:
    void Insert(const InternalKey& key, const std::string& value) {
        SkipNode* update[kMaxHeight];
        SkipNode* current = head_;

        // Traverse from top level down, recording predecessors
        int top = max_height_.load(std::memory_order_relaxed);
        for (int i = top - 1; i >= 0; --i) {
            SkipNode* next = current->forward[i].load(std::memory_order_acquire);
            while (next && next->key < key) {
                current = next;
                next = current->forward[i].load(std::memory_order_acquire);
            }
            update[i] = current;
        }

        int new_height = RandomHeight();
        if (new_height > top) {
            for (int i = top; i < new_height; ++i) {
                update[i] = head_;
            }
            // Relaxed OK: concurrent readers just see smaller height
            max_height_.store(new_height, std::memory_order_relaxed);
        }

        SkipNode* new_node = SkipNode::Create(key, value, new_height);

        // Bottom-up insertion with release ordering for readers
        for (int i = 0; i < new_height; ++i) {
            new_node->forward[i].store(
                update[i]->forward[i].load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            update[i]->forward[i].store(new_node, std::memory_order_release);
        }

        // Update bookkeeping
        size_t entry_size = key.user_key.size() + value.size() + 40;  // Overhead estimate
        approximate_size_.fetch_add(entry_size, std::memory_order_relaxed);
        entry_count_.fetch_add(1, std::memory_order_relaxed);
    }

    SkipNode* FindGreaterOrEqual(const InternalKey& key) const {
        SkipNode* current = head_;
        int top = max_height_.load(std::memory_order_acquire);
        for (int i = top - 1; i >= 0; --i) {
            SkipNode* next = current->forward[i].load(std::memory_order_acquire);
            while (next && next->key < key) {
                current = next;
                next = current->forward[i].load(std::memory_order_acquire);
            }
        }
        return current->forward[0].load(std::memory_order_acquire);
    }

    int RandomHeight() {
        static thread_local std::mt19937 rng(std::random_device{}());
        int height = 1;
        // P(height = h) = (1/4)^(h-1)
        while (height < kMaxHeight && (rng() % 4) == 0) {
            ++height;
        }
        return height;
    }

    SkipNode*               head_;
    size_t                  max_size_bytes_;
    std::atomic<size_t>     approximate_size_;
    std::atomic<size_t>     entry_count_;
    std::atomic<int>        max_height_;
    std::atomic<uint64_t>   sequence_;
};

}  // namespace storage
}  // namespace ai_kv
