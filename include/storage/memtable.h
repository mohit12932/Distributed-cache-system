#pragma once
// ────────────────────────────────────────────────────────────────
// MemTable: in-memory ordered key-value store backed by skip-list.
// Supports versioned keys (sequence numbers) and soft-deletes.
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../compat/threading.h"

namespace dcs {
namespace storage {

enum class ValueType : uint8_t {
    kValue      = 0x01,
    kDeletion   = 0x02,
};

struct InternalKey {
    std::string key;
    uint64_t    sequence;
    ValueType   type;

    bool operator<(const InternalKey& o) const {
        int cmp = key.compare(o.key);
        if (cmp != 0) return cmp < 0;
        return sequence > o.sequence;  // newer first
    }
    bool operator==(const InternalKey& o) const {
        return key == o.key && sequence == o.sequence;
    }
};

class MemTable {
public:
    static constexpr int kMaxHeight       = 12;
    static constexpr size_t kWriteBufferSize = 4 * 1024 * 1024; // 4 MB

    MemTable() : head_(new Node(kMaxHeight)), max_height_(1),
                 approx_size_(0), entry_count_(0), rng_state_(42) {}

    ~MemTable() { Clear(); }

    bool Put(const std::string& key, const std::string& value, uint64_t seq) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        InternalKey ik{key, seq, ValueType::kValue};
        Insert(ik, value);
        approx_size_ += key.size() + value.size() + 32;
        entry_count_++;
        return true;
    }

    bool Delete(const std::string& key, uint64_t seq) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        InternalKey ik{key, seq, ValueType::kDeletion};
        Insert(ik, "");
        approx_size_ += key.size() + 32;
        entry_count_++;
        return true;
    }

    // Returns {found, value, is_deletion_marker}
    struct LookupResult {
        bool found;
        std::string value;
        bool deleted;
    };

    LookupResult Get(const std::string& key) const {
        // Lock-free read via lock (safe for single-writer multiple reader)
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        Node* node = head_.get();
        for (int i = max_height_ - 1; i >= 0; i--) {
            while (node->next[i] && node->next[i]->ikey < InternalKey{key, UINT64_MAX, ValueType::kValue}) {
                node = node->next[i];
            }
        }
        Node* target = node->next[0];
        if (target && target->ikey.key == key) {
            return {true, target->value,
                    target->ikey.type == ValueType::kDeletion};
        }
        return {false, "", false};
    }

    using EntryCallback = std::function<void(const InternalKey&, const std::string&)>;

    void ForEach(EntryCallback cb) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        Node* node = head_->next[0];
        while (node) {
            cb(node->ikey, node->value);
            node = node->next[0];
        }
    }

    size_t ApproximateSize() const { return approx_size_; }
    size_t EntryCount()      const { return entry_count_; }
    bool   ShouldFlush()     const { return approx_size_ >= kWriteBufferSize; }

    void Clear() {
        compat::LockGuard<compat::Mutex> lock(mu_);
        Node* node = head_->next[0];
        while (node) {
            Node* next = node->next[0];
            delete node;
            node = next;
        }
        for (int i = 0; i < kMaxHeight; i++) head_->next[i] = nullptr;
        max_height_ = 1;
        approx_size_ = 0;
        entry_count_ = 0;
    }

private:
    struct Node {
        InternalKey       ikey;
        std::string       value;
        std::vector<Node*> next;

        explicit Node(int height) : next(height, nullptr) {}
        Node(const InternalKey& k, const std::string& v, int height)
            : ikey(k), value(v), next(height, nullptr) {}
    };

    int RandomHeight() {
        int h = 1;
        while (h < kMaxHeight && ((FastRand() & 3) == 0)) ++h;
        return h;
    }

    uint32_t FastRand() {
        rng_state_ ^= (rng_state_ << 13);
        rng_state_ ^= (rng_state_ >> 17);
        rng_state_ ^= (rng_state_ << 5);
        return rng_state_;
    }

    void Insert(const InternalKey& ikey, const std::string& value) {
        Node* prev[kMaxHeight];
        Node* node = head_.get();
        for (int i = max_height_ - 1; i >= 0; i--) {
            while (node->next[i] && node->next[i]->ikey < ikey) {
                node = node->next[i];
            }
            prev[i] = node;
        }

        // Check for exact match (same key + sequence)
        if (node->next[0] && node->next[0]->ikey == ikey) {
            node->next[0]->value = value;
            return;
        }

        int height = RandomHeight();
        if (height > max_height_) {
            for (int i = max_height_; i < height; i++) prev[i] = head_.get();
            max_height_ = height;
        }

        Node* new_node = new Node(ikey, value, height);
        for (int i = 0; i < height; i++) {
            new_node->next[i] = prev[i]->next[i];
            prev[i]->next[i] = new_node;
        }
    }

    std::unique_ptr<Node> head_;
    int                   max_height_;
    size_t                approx_size_;
    size_t                entry_count_;
    uint32_t              rng_state_;
    compat::Mutex         mu_;
};

}  // namespace storage
}  // namespace dcs
