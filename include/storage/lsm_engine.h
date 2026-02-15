#pragma once
// ────────────────────────────────────────────────────────────────
// LSM-Tree Engine: Log-Structured Merge-Tree storage backend.
// Implements StorageBackend interface with WAL → MemTable → SSTable
// pipeline and leveled compaction.
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../compat/threading.h"
#include "../persistence/storage_backend.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define DCS_MKDIR(p)  _mkdir(p)
#define DCS_ACCESS(p) _access(p, 0)
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#define DCS_MKDIR(p)  mkdir(p, 0755)
#define DCS_ACCESS(p) access(p, F_OK)
#endif

namespace dcs {
namespace storage {

struct LSMStats {
    compat::Atomic<uint64_t> wal_bytes{0};
    compat::Atomic<uint64_t> memtable_size{0};
    compat::Atomic<uint64_t> memtable_entries{0};
    compat::Atomic<uint64_t> sstable_count{0};
    compat::Atomic<uint64_t> compactions_done{0};
    compat::Atomic<uint64_t> total_puts{0};
    compat::Atomic<uint64_t> total_gets{0};
    compat::Atomic<uint64_t> total_deletes{0};
    compat::Atomic<uint64_t> bloom_filter_hits{0};
};

class LSMEngine : public persistence::StorageBackend {
public:
    static constexpr int kMaxLevels      = 4;
    static constexpr int kL0CompactTrig  = 4;
    static constexpr int kLevelMultiplier = 10;

    explicit LSMEngine(const std::string& data_dir)
        : data_dir_(data_dir), sequence_(0), running_(false),
          sstable_counter_(0) {
        EnsureDir(data_dir_);
        EnsureDir(data_dir_ + "/wal");
        EnsureDir(data_dir_ + "/sst");
        for (int i = 0; i < kMaxLevels; i++) {
            std::string level_dir = data_dir_ + "/sst/L" + std::to_string(i);
            EnsureDir(level_dir);
        }

        memtable_ = std::make_unique<MemTable>();
        wal_ = std::make_unique<WALWriter>(data_dir_ + "/wal/current.wal");
        RecoverFromWAL();
        LoadSSTables();
        running_ = true;
        compact_thread_ = compat::Thread(&LSMEngine::CompactionLoop, this);
    }

    ~LSMEngine() override {
        running_ = false;
        if (compact_thread_.joinable()) compact_thread_.join();
        FlushMemTable();
        if (wal_) wal_->Close();
    }

    // ─── StorageBackend interface ──────────────────────────────

    persistence::LoadResult load(const std::string& key) override {
        stats_.total_gets++;
        // 1. Check active memtable
        auto result = memtable_->Get(key);
        if (result.found) {
            if (result.deleted) return persistence::LoadResult::Miss();
            return persistence::LoadResult::Hit(result.value);
        }
        // 2. Check immutable memtable
        {
            compat::LockGuard<compat::Mutex> lock(imm_mu_);
            if (imm_memtable_) {
                auto imm_result = imm_memtable_->Get(key);
                if (imm_result.found) {
                    if (imm_result.deleted) return persistence::LoadResult::Miss();
                    return persistence::LoadResult::Hit(imm_result.value);
                }
            }
        }
        // 3. Check SSTables (newest first, level by level)
        compat::LockGuard<compat::Mutex> lock(sst_mu_);
        for (int level = 0; level < kMaxLevels; level++) {
            for (int i = static_cast<int>(levels_[level].size()) - 1; i >= 0; i--) {
                std::string value;
                if (levels_[level][i]->Get(key, value)) {
                    stats_.bloom_filter_hits++;
                    return persistence::LoadResult::Hit(value);
                }
            }
        }
        return persistence::LoadResult::Miss();
    }

    bool store(const std::string& key, const std::string& value) override {
        stats_.total_puts++;
        uint64_t seq = sequence_++;
        WALRecord rec{WALRecordType::kPut, key, value, seq};
        wal_->Append(rec);
        memtable_->Put(key, value, seq);
        stats_.memtable_size.store(memtable_->ApproximateSize());
        stats_.memtable_entries.store(memtable_->EntryCount());
        stats_.wal_bytes.store(wal_->BytesWritten());
        MaybeScheduleFlush();
        return true;
    }

    bool remove(const std::string& key) override {
        stats_.total_deletes++;
        uint64_t seq = sequence_++;
        WALRecord rec{WALRecordType::kDelete, key, "", seq};
        wal_->Append(rec);
        memtable_->Delete(key, seq);
        MaybeScheduleFlush();
        return true;
    }

    bool batch_store(const std::vector<std::pair<std::string, std::string>>& entries) override {
        std::vector<WALRecord> wal_batch;
        wal_batch.reserve(entries.size());
        for (const auto& e : entries) {
            uint64_t seq = sequence_++;
            wal_batch.push_back({WALRecordType::kPut, e.first, e.second, seq});
        }
        wal_->AppendBatch(wal_batch);
        for (size_t i = 0; i < entries.size(); i++) {
            memtable_->Put(entries[i].first, entries[i].second, wal_batch[i].sequence);
        }
        stats_.total_puts.fetch_add(static_cast<uint64_t>(entries.size()));
        stats_.memtable_size.store(memtable_->ApproximateSize());
        stats_.memtable_entries.store(memtable_->EntryCount());
        stats_.wal_bytes.store(wal_->BytesWritten());
        MaybeScheduleFlush();
        return true;
    }

    bool ping() override { return running_; }

    // ─── Statistics ────────────────────────────────────────────

    const LSMStats& Stats() const { return stats_; }

    // Force a compaction (for demo purposes)
    void ForceCompaction() {
        {
            compat::LockGuard<compat::Mutex> lock(imm_mu_);
            if (memtable_->EntryCount() > 0) {
                imm_memtable_ = std::move(memtable_);
                memtable_ = std::make_unique<MemTable>();
                flush_pending_ = true;
            }
            if (flush_pending_ && imm_memtable_) DoFlush();
        }
        bool needs = false;
        { compat::LockGuard<compat::Mutex> lock(sst_mu_); needs = !levels_[0].empty(); }
        if (needs) { CompactLevel(0); stats_.compactions_done++; }
    }

    size_t SSTCountAtLevel(int level) const {
        if (level < 0 || level >= kMaxLevels) return 0;
        return levels_[level].size();
    }

    size_t TotalSSTCount() const {
        size_t total = 0;
        for (int i = 0; i < kMaxLevels; i++) total += levels_[i].size();
        return total;
    }

private:
    void MaybeScheduleFlush() {
        if (memtable_->ShouldFlush()) {
            compat::LockGuard<compat::Mutex> lock(imm_mu_);
            if (!imm_memtable_) {
                imm_memtable_ = std::move(memtable_);
                memtable_ = std::make_unique<MemTable>();
                // Rotate WAL
                wal_->Close();
                std::string old_wal = data_dir_ + "/wal/current.wal";
                std::string new_wal = data_dir_ + "/wal/rotating_" +
                    std::to_string(sequence_.load()) + ".wal";
                std::rename(old_wal.c_str(), new_wal.c_str());
                wal_ = std::make_unique<WALWriter>(data_dir_ + "/wal/current.wal");
                flush_pending_ = true;
            }
        }
    }

    void FlushMemTable() {
        compat::LockGuard<compat::Mutex> lock(imm_mu_);
        if (!imm_memtable_ && memtable_->EntryCount() > 0) {
            imm_memtable_ = std::move(memtable_);
            memtable_ = std::make_unique<MemTable>();
        }
        if (imm_memtable_) {
            DoFlush();
        }
    }

    void DoFlush() {
        // imm_mu_ must be held by caller
        if (!imm_memtable_) return;
        uint64_t counter = sstable_counter_++;
        std::string sst_path = data_dir_ + "/sst/L0/sst_" +
            std::to_string(counter) + ".sst";
        SSTableWriter writer(sst_path);
        imm_memtable_->ForEach([&](const InternalKey& ik, const std::string& val) {
            if (ik.type == ValueType::kValue) {
                writer.Add(ik.key, val);
            }
        });
        writer.Finish();
        {
            compat::LockGuard<compat::Mutex> lock(sst_mu_);
            levels_[0].push_back(std::make_shared<SSTableReader>(sst_path));
            stats_.sstable_count.store(TotalSSTCount());
        }
        imm_memtable_.reset();
        flush_pending_ = false;
        // Delete rotated WAL files
        CleanupRotatedWALs();
    }

    void CompactionLoop() {
        while (running_) {
            {
                compat::LockGuard<compat::Mutex> lock(imm_mu_);
                if (flush_pending_ && imm_memtable_) {
                    DoFlush();
                }
            }

            // Check if L0 compaction needed
            bool needs_compact = false;
            {
                compat::LockGuard<compat::Mutex> lock(sst_mu_);
                needs_compact = (levels_[0].size() >= static_cast<size_t>(kL0CompactTrig));
            }
            if (needs_compact) {
                CompactLevel(0);
                stats_.compactions_done++;
            }
            compat::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void CompactLevel(int level) {
        if (level >= kMaxLevels - 1) return;
        compat::LockGuard<compat::Mutex> lock(sst_mu_);
        if (levels_[level].empty()) return;

        // Merge all SSTables at this level into next level
        std::unordered_map<std::string, std::string> merged;
        for (auto& sst : levels_[level]) {
            auto keys = sst->AllKeys();
            for (const auto& key : keys) {
                std::string val;
                if (sst->Get(key, val)) {
                    merged[key] = val;
                }
            }
        }
        // Include existing next-level tables
        for (auto& sst : levels_[level + 1]) {
            auto keys = sst->AllKeys();
            for (const auto& key : keys) {
                if (merged.find(key) == merged.end()) {
                    std::string val;
                    if (sst->Get(key, val)) merged[key] = val;
                }
            }
        }

        // Write merged SSTable
        uint64_t counter = sstable_counter_++;
        std::string sst_path = data_dir_ + "/sst/L" + std::to_string(level + 1) +
            "/sst_" + std::to_string(counter) + ".sst";
        SSTableWriter writer(sst_path);
        for (const auto& kv : merged) {
            writer.Add(kv.first, kv.second);
        }
        writer.Finish();

        // Remove old SSTables (delete files)
        for (auto& sst : levels_[level]) {
            std::remove(sst->Filepath().c_str());
        }
        for (auto& sst : levels_[level + 1]) {
            std::remove(sst->Filepath().c_str());
        }
        levels_[level].clear();
        levels_[level + 1].clear();
        levels_[level + 1].push_back(std::make_shared<SSTableReader>(sst_path));
        stats_.sstable_count.store(TotalSSTCount());
    }

    void RecoverFromWAL() {
        std::string wal_path = data_dir_ + "/wal/current.wal";
        WALReader reader(wal_path);
        reader.Replay([this](const WALRecord& rec) {
            if (rec.sequence >= sequence_) sequence_ = rec.sequence + 1;
            if (rec.type == WALRecordType::kPut) {
                memtable_->Put(rec.key, rec.value, rec.sequence);
            } else if (rec.type == WALRecordType::kDelete) {
                memtable_->Delete(rec.key, rec.sequence);
            }
        });
    }

    void LoadSSTables() {
        // Scan sst directories for existing files
        // On Windows we use _findfirst/_findnext pattern
        for (int level = 0; level < kMaxLevels; level++) {
            std::string dir = data_dir_ + "/sst/L" + std::to_string(level);
            LoadSSTFromDir(dir, level);
        }
        stats_.sstable_count.store(TotalSSTCount());
    }

    void LoadSSTFromDir(const std::string& dir, int level) {
#ifdef _WIN32
        struct _finddata_t fileinfo;
        std::string pattern = dir + "/*.sst";
        intptr_t handle = _findfirst(pattern.c_str(), &fileinfo);
        if (handle == -1) return;
        do {
            std::string filepath = dir + "/" + fileinfo.name;
            auto reader = std::make_shared<SSTableReader>(filepath);
            if (reader->Valid()) {
                levels_[level].push_back(reader);
            }
        } while (_findnext(handle, &fileinfo) == 0);
        _findclose(handle);
#else
        // POSIX: use opendir/readdir
        DIR* d = opendir(dir.c_str());
        if (!d) return;
        struct dirent* entry;
        while ((entry = readdir(d)) != nullptr) {
            std::string name(entry->d_name);
            if (name.size() > 4 && name.substr(name.size() - 4) == ".sst") {
                std::string filepath = dir + "/" + name;
                auto reader = std::make_shared<SSTableReader>(filepath);
                if (reader->Valid()) levels_[level].push_back(reader);
            }
        }
        closedir(d);
#endif
    }

    void CleanupRotatedWALs() {
#ifdef _WIN32
        struct _finddata_t fileinfo;
        std::string pattern = data_dir_ + "/wal/rotating_*.wal";
        intptr_t handle = _findfirst(pattern.c_str(), &fileinfo);
        if (handle == -1) return;
        do {
            std::string filepath = data_dir_ + "/wal/" + fileinfo.name;
            std::remove(filepath.c_str());
        } while (_findnext(handle, &fileinfo) == 0);
        _findclose(handle);
#else
        // Simple cleanup with system call
        std::string cmd = "rm -f " + data_dir_ + "/wal/rotating_*.wal";
        (void)system(cmd.c_str());
#endif
    }

    static void EnsureDir(const std::string& path) {
        if (DCS_ACCESS(path.c_str()) != 0) {
            DCS_MKDIR(path.c_str());
        }
    }

    std::string data_dir_;
    compat::Atomic<uint64_t> sequence_;
    compat::Atomic<bool>     running_;
    compat::Atomic<uint64_t> sstable_counter_;
    bool flush_pending_ = false;

    std::unique_ptr<MemTable>   memtable_;
    std::unique_ptr<MemTable>   imm_memtable_;
    std::unique_ptr<WALWriter>  wal_;

    std::vector<std::shared_ptr<SSTableReader>> levels_[kMaxLevels];

    compat::Mutex mu_;
    compat::Mutex imm_mu_;
    compat::Mutex sst_mu_;

    compat::Thread compact_thread_;
    LSMStats    stats_;
};

}  // namespace storage
}  // namespace dcs
