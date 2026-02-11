// ai_kv_store/include/storage/lsm_tree.h
// ────────────────────────────────────────────────────────────────
// LSM-Tree: Orchestrates MemTable, WAL, SSTables, and compaction.
// Write path: WAL → MemTable → flush → Level-0 SSTables
// Read path:  MemTable → Level-0 (newest first) → Level-1+ → miss
// ────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"

#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace ai_kv {
namespace storage {

// ── Configuration ──

struct LSMConfig {
    std::string data_dir           = "./data";
    size_t      memtable_size      = 4 * 1024 * 1024;     // 4 MB
    size_t      block_size         = 4096;                 // SSTable block size
    int         level0_stop_writes = 8;                    // Slow down at this many L0 files
    int         level0_compaction  = 4;                    // Start compaction at this many L0 files
    int         max_levels         = 7;
    int         size_ratio         = 10;                   // Level size multiplier
    size_t      base_level_size    = 64 * 1024 * 1024;     // 64 MB for Level-1
};

// ── SSTable Metadata ──

struct SSTableMeta {
    int         level;
    uint64_t    file_number;
    std::string smallest_key;
    std::string largest_key;
    size_t      file_size;
    size_t      entry_count;

    std::string Filepath(const std::string& data_dir) const {
        return absl::StrCat(data_dir, "/L", level, "_", file_number, ".sst");
    }
};

// ── Version: snapshot of the SSTable manifest at a point in time ──

struct Version {
    // levels_[i] = list of SSTables at level i, sorted by key range
    std::vector<std::vector<SSTableMeta>> levels;

    explicit Version(int max_levels) : levels(max_levels) {}

    int NumLevelFiles(int level) const {
        if (level < 0 || level >= static_cast<int>(levels.size())) return 0;
        return static_cast<int>(levels[level].size());
    }

    size_t LevelSize(int level) const {
        size_t total = 0;
        for (const auto& m : levels[level]) total += m.file_size;
        return total;
    }
};

// ── LSM-Tree Engine ──

class LSMTree {
public:
    explicit LSMTree(const LSMConfig& config = LSMConfig{})
        : config_(config),
          next_file_number_(1),
          current_version_(std::make_shared<Version>(config.max_levels)),
          shutdown_(false) {
        // Create data directory (platform-specific, simplified)
        std::string mkdir_cmd = "mkdir -p " + config_.data_dir;
        (void)system(mkdir_cmd.c_str());

        // Initialize active MemTable + WAL
        RotateMemTable();
    }

    ~LSMTree() {
        shutdown_.store(true, std::memory_order_release);
        if (wal_) wal_->Close();
    }

    // ── Public Write API ──

    bool Put(absl::string_view key, absl::string_view value) {
        absl::MutexLock lock(&write_mu_);

        // Write to WAL first (crash safety)
        WALRecord rec{WALRecordType::kPut, std::string(key), std::string(value), 0};
        if (wal_ && !wal_->Append(rec)) return false;

        // Insert into active MemTable
        active_memtable_->Put(key, value);

        // Check if MemTable needs flushing
        MaybeScheduleFlush();
        return true;
    }

    bool Delete(absl::string_view key) {
        absl::MutexLock lock(&write_mu_);

        WALRecord rec{WALRecordType::kDelete, std::string(key), "", 0};
        if (wal_ && !wal_->Append(rec)) return false;

        active_memtable_->Delete(key);
        MaybeScheduleFlush();
        return true;
    }

    // ── Public Read API ──

    struct GetResult {
        bool        found;
        std::string value;
    };

    GetResult Get(absl::string_view key) const {
        // 1. Check active MemTable
        {
            auto result = active_memtable_->Get(key);
            if (result.found) {
                if (result.is_deletion) return {false, ""};
                return {true, result.value};
            }
        }

        // 2. Check immutable MemTables (newest first)
        {
            absl::ReaderMutexLock lock(&version_mu_);
            for (auto it = immutable_memtables_.rbegin();
                 it != immutable_memtables_.rend(); ++it) {
                auto result = (*it)->Get(key);
                if (result.found) {
                    if (result.is_deletion) return {false, ""};
                    return {true, result.value};
                }
            }
        }

        // 3. Check SSTables level by level
        auto version = GetCurrentVersion();
        return SearchSSTables(*version, key);
    }

    // ── Compaction (called by background thread) ──

    // Flush the oldest immutable MemTable to a Level-0 SSTable.
    bool FlushImmutableMemTable() {
        std::shared_ptr<MemTable> to_flush;
        {
            absl::MutexLock lock(&version_mu_);
            if (immutable_memtables_.empty()) return false;
            to_flush = immutable_memtables_.front();
        }

        // Write all entries to a new SSTable
        uint64_t file_num = next_file_number_.fetch_add(1);
        SSTableMeta meta;
        meta.level       = 0;
        meta.file_number = file_num;
        meta.entry_count = 0;

        std::string filepath = meta.Filepath(config_.data_dir);
        SSTableWriter writer(filepath, config_.block_size, to_flush->EntryCount());

        std::string first_key, last_key;
        to_flush->ForEach([&](const InternalKey& k, const std::string& v) {
            writer.Add(k, v);
            if (first_key.empty()) first_key = k.user_key;
            last_key = k.user_key;
        });

        meta.file_size    = writer.Finish();
        meta.entry_count  = writer.EntryCount();
        meta.smallest_key = first_key;
        meta.largest_key  = last_key;

        // Install new version with the flushed SSTable
        {
            absl::MutexLock lock(&version_mu_);
            auto new_version = std::make_shared<Version>(*current_version_);
            new_version->levels[0].push_back(meta);
            current_version_ = new_version;
            immutable_memtables_.pop_front();
        }

        return true;
    }

    // Leveled compaction: merge overlapping SSTables from L to L+1.
    bool CompactLevel(int level) {
        if (level + 1 >= config_.max_levels) return false;

        auto version = GetCurrentVersion();
        if (version->levels[level].empty()) return false;

        // Pick the oldest SSTable from `level`
        const SSTableMeta& source = version->levels[level].front();

        // Find overlapping SSTables in level+1
        std::vector<size_t> overlapping_indices;
        for (size_t i = 0; i < version->levels[level + 1].size(); ++i) {
            const auto& target = version->levels[level + 1][i];
            if (target.smallest_key <= source.largest_key &&
                target.largest_key >= source.smallest_key) {
                overlapping_indices.push_back(i);
            }
        }

        // Open readers for all input SSTables
        std::vector<std::unique_ptr<SSTableReader>> readers;
        auto src_reader = SSTableReader::Open(source.Filepath(config_.data_dir));
        if (!src_reader) return false;
        readers.push_back(std::move(src_reader));

        for (size_t idx : overlapping_indices) {
            auto r = SSTableReader::Open(
                version->levels[level + 1][idx].Filepath(config_.data_dir));
            if (r) readers.push_back(std::move(r));
        }

        // TODO: Implement full merge-sort of readers into new SSTable(s)
        // This is a placeholder for the merge-sort compaction logic.
        // In production:
        //   1. Merge-sort all entries from input SSTables
        //   2. Write one or more output SSTables at level+1
        //   3. Atomically swap the version (remove inputs, add outputs)
        //   4. Delete obsolete SSTable files

        return true;
    }

    // ── Stats ──

    struct Stats {
        size_t active_memtable_size;
        size_t immutable_count;
        std::vector<int> files_per_level;
        uint64_t next_file_number;
    };

    Stats GetStats() const {
        Stats s;
        s.active_memtable_size = active_memtable_->ApproximateSize();
        {
            absl::ReaderMutexLock lock(&version_mu_);
            s.immutable_count = immutable_memtables_.size();
        }
        auto ver = GetCurrentVersion();
        for (int i = 0; i < config_.max_levels; ++i) {
            s.files_per_level.push_back(ver->NumLevelFiles(i));
        }
        s.next_file_number = next_file_number_.load();
        return s;
    }

private:
    void RotateMemTable() {
        active_memtable_ = std::make_shared<MemTable>(config_.memtable_size);
        uint64_t wal_num = next_file_number_.fetch_add(1);
        std::string wal_path = absl::StrCat(config_.data_dir, "/", wal_num, ".wal");
        wal_ = std::make_unique<WALWriter>(wal_path);
    }

    void MaybeScheduleFlush() {
        if (!active_memtable_->ShouldFlush()) return;

        // Freeze current MemTable, push to immutable queue
        {
            absl::MutexLock lock(&version_mu_);
            immutable_memtables_.push_back(active_memtable_);
        }

        // Start fresh MemTable + WAL
        RotateMemTable();

        // In production: signal background flush thread via condition variable
    }

    GetResult SearchSSTables(const Version& ver, absl::string_view key) const {
        // Level 0: Check all SSTables (may overlap), newest first
        for (int i = static_cast<int>(ver.levels[0].size()) - 1; i >= 0; --i) {
            auto reader = SSTableReader::Open(
                ver.levels[0][i].Filepath(config_.data_dir));
            if (!reader) continue;
            auto result = reader->Get(key);
            if (result.found) {
                if (result.is_deletion) return {false, ""};
                return {true, result.value};
            }
        }

        // Level 1+: Binary search (non-overlapping, sorted)
        for (int level = 1; level < config_.max_levels; ++level) {
            const auto& files = ver.levels[level];
            if (files.empty()) continue;

            // Binary search for the SSTable whose range contains key
            auto it = std::lower_bound(
                files.begin(), files.end(), key,
                [](const SSTableMeta& m, absl::string_view k) {
                    return m.largest_key < k;
                });

            if (it == files.end()) continue;
            if (it->smallest_key > key) continue;

            auto reader = SSTableReader::Open(it->Filepath(config_.data_dir));
            if (!reader) continue;
            auto result = reader->Get(key);
            if (result.found) {
                if (result.is_deletion) return {false, ""};
                return {true, result.value};
            }
        }

        return {false, ""};
    }

    std::shared_ptr<Version> GetCurrentVersion() const {
        absl::ReaderMutexLock lock(&version_mu_);
        return current_version_;
    }

    LSMConfig                                config_;
    std::atomic<uint64_t>                    next_file_number_;

    // Active write state
    absl::Mutex                              write_mu_;
    std::shared_ptr<MemTable>                active_memtable_;
    std::unique_ptr<WALWriter>               wal_;

    // Version management
    mutable absl::Mutex                      version_mu_;
    std::deque<std::shared_ptr<MemTable>>    immutable_memtables_ ABSL_GUARDED_BY(version_mu_);
    std::shared_ptr<Version>                 current_version_     ABSL_GUARDED_BY(version_mu_);

    std::atomic<bool>                        shutdown_;
};

}  // namespace storage
}  // namespace ai_kv
