#pragma once
// ────────────────────────────────────────────────────────────────
// Raft Log: Persistent log entries and voting state for Raft.
// File-backed with append-only writes.
// ────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "../compat/threading.h"

namespace dcs {
namespace raft {

struct LogEntry {
    uint64_t    term;
    uint64_t    index;
    std::string command;  // serialized KV operation
};

struct PersistentState {
    uint64_t    current_term = 0;
    int         voted_for    = -1;  // -1 = none
};

class RaftLog {
public:
    explicit RaftLog(const std::string& data_dir = "data/raft")
        : data_dir_(data_dir), base_index_(0) {
        EnsureDir(data_dir_);
        LoadState();
        LoadEntries();
        std::string path = data_dir_ + "/raft_log.dat";
        log_file_.open(path, std::ios::binary | std::ios::app);
    }

    ~RaftLog() {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (log_file_.is_open()) {
            log_file_.flush();
            log_file_.close();
        }
    }

    // ─── Persistent State ──────────────────────────────────────

    uint64_t CurrentTerm() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        return state_.current_term;
    }

    int VotedFor() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        return state_.voted_for;
    }

    void SetTerm(uint64_t term) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        state_.current_term = term;
        state_.voted_for = -1;
        SaveState();
    }

    void SetVotedFor(int candidate) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        state_.voted_for = candidate;
        SaveState();
    }

    // ─── Log Entries ───────────────────────────────────────────

    size_t Size() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        return entries_.size();
    }

    uint64_t LastIndex() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        if (entries_.empty()) return 0;
        return entries_.back().index;
    }

    uint64_t LastTerm() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        if (entries_.empty()) return 0;
        return entries_.back().term;
    }

    bool GetEntry(uint64_t index, LogEntry& out) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        if (index < base_index_ || entries_.empty()) return false;
        size_t pos = index - base_index_;
        if (pos >= entries_.size()) return false;
        out = entries_[pos];
        return true;
    }

    uint64_t TermAt(uint64_t index) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        if (index == 0) return 0;
        if (index < base_index_ || entries_.empty()) return 0;
        size_t pos = index - base_index_;
        if (pos >= entries_.size()) return 0;
        return entries_[pos].term;
    }

    void Append(const LogEntry& entry) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        entries_.push_back(entry);
        if (entries_.size() == 1) base_index_ = entry.index;
        AppendEntryToFile(entry);
    }

    void AppendBatch(const std::vector<LogEntry>& batch) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        for (const auto& entry : batch) {
            entries_.push_back(entry);
            if (entries_.size() == 1) base_index_ = entry.index;
            AppendEntryToFile(entry);
        }
    }

    // Truncate log from index onwards (inclusive)
    void TruncateFrom(uint64_t index) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (index == 0 || entries_.empty()) return;
        // Remove all entries with log index >= the given index.
        // After compaction, entries may not start at index 1,
        // so we cannot use index as an array position.
        size_t keep = 0;
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].index < index) keep = i + 1;
            else break;  // entries are sorted by index
        }
        if (keep < entries_.size()) {
            entries_.resize(keep);
            if (entries_.empty()) base_index_ = 0;
            RewriteLog();
        }
    }

    // Compact log: remove entries before compact_index (keep from compact_index onward)
    void CompactBefore(uint64_t compact_index) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (compact_index <= 1 || entries_.empty()) return;
        // Find how many entries to remove (entries are 1-indexed)
        size_t remove_count = 0;
        for (size_t i = 0; i < entries_.size(); i++) {
            if (entries_[i].index < compact_index) remove_count++;
            else break;
        }
        if (remove_count == 0) return;
        entries_.erase(entries_.begin(), entries_.begin() + remove_count);
        // Shrink to fit to actually free memory
        entries_.shrink_to_fit();
        if (!entries_.empty()) {
            base_index_ = entries_.front().index;
        } else {
            base_index_ = 0;
        }
        RewriteLog();
    }

    // Get entries from start_index to end (for replication), capped at max_entries
    std::vector<LogEntry> GetRange(uint64_t start_index, size_t max_entries = 500) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        std::vector<LogEntry> result;
        size_t count = 0;
        for (size_t i = 0; i < entries_.size() && count < max_entries; i++) {
            if (entries_[i].index >= start_index) {
                result.push_back(entries_[i]);
                count++;
            }
        }
        return result;
    }

    bool MatchesAt(uint64_t index, uint64_t term) const {
        if (index == 0) return true;  // empty log matches anything
        uint64_t t = TermAt(index);
        if (t == 0) return true;  // entry was compacted, assume match
        return t == term;
    }

private:
    void LoadState() {
        std::string path = data_dir_ + "/raft_state.dat";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        f.read(reinterpret_cast<char*>(&state_), sizeof(PersistentState));
    }

    void SaveState() {
        std::string path = data_dir_ + "/raft_state.dat";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        f.write(reinterpret_cast<const char*>(&state_), sizeof(PersistentState));
        f.flush();
    }

    void LoadEntries() {
        std::string path = data_dir_ + "/raft_log.dat";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        while (f.good() && !f.eof()) {
            LogEntry entry;
            f.read(reinterpret_cast<char*>(&entry.term), 8);
            f.read(reinterpret_cast<char*>(&entry.index), 8);
            uint32_t cmd_len = 0;
            f.read(reinterpret_cast<char*>(&cmd_len), 4);
            if (!f.good() || cmd_len > 64 * 1024 * 1024) break;
            entry.command.resize(cmd_len);
            f.read(&entry.command[0], cmd_len);
            if (f.good()) entries_.push_back(entry);
        }
        if (!entries_.empty()) {
            base_index_ = entries_.front().index;
        } else {
            base_index_ = 0;
        }
    }

    void AppendEntryToFile(const LogEntry& entry) {
        if (!log_file_.is_open()) return;
        log_file_.write(reinterpret_cast<const char*>(&entry.term), 8);
        log_file_.write(reinterpret_cast<const char*>(&entry.index), 8);
        uint32_t cmd_len = static_cast<uint32_t>(entry.command.size());
        log_file_.write(reinterpret_cast<const char*>(&cmd_len), 4);
        log_file_.write(entry.command.data(), cmd_len);
        log_file_.flush();
    }

    void RewriteLog() {
        if (log_file_.is_open()) {
            log_file_.close();
        }
        // Rewrite entire log file after truncation
        std::string path = data_dir_ + "/raft_log.dat";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        for (const auto& entry : entries_) {
            f.write(reinterpret_cast<const char*>(&entry.term), 8);
            f.write(reinterpret_cast<const char*>(&entry.index), 8);
            uint32_t cmd_len = static_cast<uint32_t>(entry.command.size());
            f.write(reinterpret_cast<const char*>(&cmd_len), 4);
            f.write(entry.command.data(), cmd_len);
        }
        f.flush();
        f.close();
        log_file_.open(path, std::ios::binary | std::ios::app);
    }

    static void EnsureDir(const std::string& path) {
#ifdef _WIN32
        _mkdir(path.c_str());
#else
        mkdir(path.c_str(), 0755);
#endif
    }

    std::string          data_dir_;
    PersistentState      state_;
    std::vector<LogEntry> entries_;
    uint64_t             base_index_;
    std::ofstream        log_file_;
    compat::Mutex        mu_;
};

}  // namespace raft
}  // namespace dcs
