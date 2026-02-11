// ai_kv_store/include/core/shard_manager.h
// ────────────────────────────────────────────────────────────────
// Consistent Hash Ring with virtual nodes for shard routing.
// Maps keys to shard IDs, supports dynamic migration.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "absl/hash/hash.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

namespace ai_kv {
namespace core {

// ── Shard descriptor ──

struct ShardInfo {
    uint32_t    id;
    uint32_t    node_id;       // Which physical node owns this shard
    std::string node_address;  // "host:port"
    uint64_t    key_count;
    uint64_t    byte_size;
    bool        migrating;     // True during active migration
};

// ── Consistent Hash Ring ──

class ConsistentHashRing {
public:
    explicit ConsistentHashRing(int virtual_nodes_per_shard = 150)
        : vnodes_per_shard_(virtual_nodes_per_shard) {}

    // Add a shard to the ring
    void AddShard(const ShardInfo& shard) {
        absl::MutexLock lock(&mu_);
        shards_[shard.id] = shard;
        for (int v = 0; v < vnodes_per_shard_; ++v) {
            uint64_t hash = VNodeHash(shard.id, v);
            ring_[hash] = shard.id;
        }
    }

    // Remove a shard from the ring
    void RemoveShard(uint32_t shard_id) {
        absl::MutexLock lock(&mu_);
        shards_.erase(shard_id);
        for (int v = 0; v < vnodes_per_shard_; ++v) {
            uint64_t hash = VNodeHash(shard_id, v);
            ring_.erase(hash);
        }
    }

    // Route a key to its shard
    uint32_t GetShard(absl::string_view key) const {
        absl::ReaderMutexLock lock(&mu_);
        if (ring_.empty()) return 0;

        uint64_t hash = KeyHash(key);
        auto it = ring_.lower_bound(hash);
        if (it == ring_.end()) {
            it = ring_.begin();  // Wrap around
        }
        return it->second;
    }

    // Get the node address for a key
    std::string GetNodeAddress(absl::string_view key) const {
        absl::ReaderMutexLock lock(&mu_);
        uint32_t shard_id = GetShardUnlocked(key);
        auto it = shards_.find(shard_id);
        return (it != shards_.end()) ? it->second.node_address : "";
    }

    // Migrate a key range: remap keys from source_shard to target_shard
    void MigrateKeyRange(absl::string_view start_key, absl::string_view end_key,
                         uint32_t source_shard, uint32_t target_shard) {
        absl::MutexLock lock(&mu_);
        // Add override entries for the migrated key range
        OverrideEntry entry;
        entry.start_key    = std::string(start_key);
        entry.end_key      = std::string(end_key);
        entry.target_shard = target_shard;
        overrides_.push_back(entry);
    }

    // Get shard info
    ShardInfo GetShardInfo(uint32_t shard_id) const {
        absl::ReaderMutexLock lock(&mu_);
        auto it = shards_.find(shard_id);
        if (it != shards_.end()) return it->second;
        return {};
    }

    // List all shards
    std::vector<ShardInfo> ListShards() const {
        absl::ReaderMutexLock lock(&mu_);
        std::vector<ShardInfo> result;
        for (const auto& kv : shards_) {
            result.push_back(kv.second);
        }
        return result;
    }

    size_t ShardCount() const {
        absl::ReaderMutexLock lock(&mu_);
        return shards_.size();
    }

private:
    struct OverrideEntry {
        std::string start_key;
        std::string end_key;
        uint32_t    target_shard;
    };

    uint32_t GetShardUnlocked(absl::string_view key) const {
        // Check overrides first (migration redirects)
        for (const auto& ov : overrides_) {
            if (key >= ov.start_key && key < ov.end_key) {
                return ov.target_shard;
            }
        }

        if (ring_.empty()) return 0;
        uint64_t hash = KeyHash(key);
        auto it = ring_.lower_bound(hash);
        if (it == ring_.end()) it = ring_.begin();
        return it->second;
    }

    static uint64_t KeyHash(absl::string_view key) {
        return absl::HashOf(key);
    }

    static uint64_t VNodeHash(uint32_t shard_id, int vnode) {
        return absl::HashOf(absl::StrCat("shard:", shard_id, ":vn:", vnode));
    }

    mutable absl::Mutex                         mu_;
    int                                         vnodes_per_shard_;
    std::map<uint64_t, uint32_t>                ring_   ABSL_GUARDED_BY(mu_);
    std::unordered_map<uint32_t, ShardInfo>     shards_ ABSL_GUARDED_BY(mu_);
    std::vector<OverrideEntry>                  overrides_ ABSL_GUARDED_BY(mu_);
};

}  // namespace core
}  // namespace ai_kv
