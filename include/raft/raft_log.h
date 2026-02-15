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
        : data_dir_(data_dir) {
        EnsureDir(data_dir_);
        LoadState();
        LoadEntries();
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
        if (index == 0 || index > entries_.size()) return false;
        out = entries_[index - 1];
        return true;
    }

    uint64_t TermAt(uint64_t index) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        if (index == 0 || index > entries_.size()) return 0;
        return entries_[index - 1].term;
    }

    void Append(const LogEntry& entry) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        entries_.push_back(entry);
        AppendEntryToFile(entry);
    }

    void AppendBatch(const std::vector<LogEntry>& batch) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        for (const auto& entry : batch) {
            entries_.push_back(entry);
            AppendEntryToFile(entry);
        }
    }

    // Truncate log from index onwards (inclusive)
    void TruncateFrom(uint64_t index) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (index == 0 || index > entries_.size()) return;
        entries_.resize(index - 1);
        RewriteLog();
    }

    // Get entries from start_index to end (for replication)
    std::vector<LogEntry> GetRange(uint64_t start_index) const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        std::vector<LogEntry> result;
        for (size_t i = start_index; i <= entries_.size(); i++) {
            result.push_back(entries_[i - 1]);
        }
        return result;
    }

    bool MatchesAt(uint64_t index, uint64_t term) const {
        if (index == 0) return true;  // empty log matches anything
        return TermAt(index) == term;
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
    }

    void AppendEntryToFile(const LogEntry& entry) {
        std::string path = data_dir_ + "/raft_log.dat";
        std::ofstream f(path, std::ios::binary | std::ios::app);
        f.write(reinterpret_cast<const char*>(&entry.term), 8);
        f.write(reinterpret_cast<const char*>(&entry.index), 8);
        uint32_t cmd_len = static_cast<uint32_t>(entry.command.size());
        f.write(reinterpret_cast<const char*>(&cmd_len), 4);
        f.write(entry.command.data(), cmd_len);
        f.flush();
    }

    void RewriteLog() {
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
    compat::Mutex        mu_;
};

}  // namespace raft
}  // namespace dcs
