// ai_kv_store/include/raft/raft_log.h
// ────────────────────────────────────────────────────────────────
// Raft persistent log: stores LogEntry records durably.
// Backed by a simple append-only file + in-memory index.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstring>
#include <deque>
#include <fstream>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

namespace ai_kv {
namespace raft {

// ── Log entry types (mirrors proto LogEntryType) ──

enum class EntryType : uint8_t {
    kNormal     = 0,
    kConfig     = 1,
    kShardMove  = 2,
    kNoop       = 3,
};

struct LogEntry {
    uint64_t  term;
    uint64_t  index;
    EntryType type;
    std::string command;  // Opaque serialized payload

    // Serialization for on-disk storage
    std::string Encode() const {
        std::string buf;
        buf.reserve(8 + 8 + 1 + 4 + command.size());
        buf.append(reinterpret_cast<const char*>(&term), 8);
        buf.append(reinterpret_cast<const char*>(&index), 8);
        buf.push_back(static_cast<char>(type));
        uint32_t cmd_len = static_cast<uint32_t>(command.size());
        buf.append(reinterpret_cast<const char*>(&cmd_len), 4);
        buf.append(command);
        return buf;
    }

    static LogEntry Decode(const char* data, size_t len) {
        LogEntry e;
        size_t pos = 0;
        std::memcpy(&e.term, data + pos, 8); pos += 8;
        std::memcpy(&e.index, data + pos, 8); pos += 8;
        e.type = static_cast<EntryType>(data[pos++]);
        uint32_t cmd_len = 0;
        std::memcpy(&cmd_len, data + pos, 4); pos += 4;
        e.command = std::string(data + pos, cmd_len);
        return e;
    }
};

// ── Persistent Raft state (voted_for, current_term) ──

struct PersistentState {
    uint64_t current_term = 0;
    int32_t  voted_for    = -1;  // -1 = none

    std::string Encode() const {
        std::string buf(12, '\0');
        std::memcpy(&buf[0], &current_term, 8);
        std::memcpy(&buf[8], &voted_for, 4);
        return buf;
    }

    static PersistentState Decode(const char* data) {
        PersistentState s;
        std::memcpy(&s.current_term, data, 8);
        std::memcpy(&s.voted_for, data + 8, 4);
        return s;
    }
};

// ── Raft Log: in-memory with file backing ──

class RaftLog {
public:
    explicit RaftLog(const std::string& log_dir)
        : log_dir_(log_dir),
          first_index_(1),
          last_applied_(0) {
        // Load state from disk on construction
        LoadState();
        LoadEntries();
    }

    // ── Append ──

    void Append(const LogEntry& entry) {
        absl::MutexLock lock(&mu_);
        assert(entry.index == FirstIndex() + entries_.size());
        entries_.push_back(entry);
        PersistEntry(entry);
    }

    void AppendBatch(const std::vector<LogEntry>& entries) {
        absl::MutexLock lock(&mu_);
        for (const auto& e : entries) {
            entries_.push_back(e);
            PersistEntry(e);
        }
    }

    // ── Access ──

    // Get entry at absolute index. Returns nullptr if out of range.
    const LogEntry* Entry(uint64_t index) const {
        absl::ReaderMutexLock lock(&mu_);
        if (index < first_index_ || index >= first_index_ + entries_.size()) {
            return nullptr;
        }
        return &entries_[index - first_index_];
    }

    uint64_t LastIndex() const {
        absl::ReaderMutexLock lock(&mu_);
        return entries_.empty() ? 0 : first_index_ + entries_.size() - 1;
    }

    uint64_t LastTerm() const {
        absl::ReaderMutexLock lock(&mu_);
        return entries_.empty() ? 0 : entries_.back().term;
    }

    uint64_t TermAt(uint64_t index) const {
        const LogEntry* e = Entry(index);
        return e ? e->term : 0;
    }

    uint64_t FirstIndex() const { return first_index_; }

    // ── Conflict resolution ──

    // Truncate log from `from_index` onward (for conflict on AppendEntries)
    void TruncateFrom(uint64_t from_index) {
        absl::MutexLock lock(&mu_);
        if (from_index < first_index_) return;
        size_t offset = from_index - first_index_;
        if (offset < entries_.size()) {
            entries_.erase(entries_.begin() + offset, entries_.end());
        }
        // In production: also truncate the on-disk log file
    }

    // ── Entries for replication ──

    // Get entries in range [from, to] inclusive
    std::vector<LogEntry> Slice(uint64_t from, uint64_t to) const {
        absl::ReaderMutexLock lock(&mu_);
        std::vector<LogEntry> result;
        for (uint64_t i = from; i <= to; ++i) {
            size_t offset = i - first_index_;
            if (offset < entries_.size()) {
                result.push_back(entries_[offset]);
            }
        }
        return result;
    }

    // ── Persistent state ──

    PersistentState GetState() const {
        absl::ReaderMutexLock lock(&mu_);
        return state_;
    }

    void SetState(const PersistentState& s) {
        absl::MutexLock lock(&mu_);
        state_ = s;
        PersistState();
    }

    // ── Application tracking ──

    uint64_t LastApplied() const {
        return last_applied_;
    }

    void SetLastApplied(uint64_t index) {
        last_applied_ = index;
    }

private:
    void LoadState() {
        std::string path = log_dir_ + "/raft_state";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        char buf[12];
        f.read(buf, 12);
        if (f.gcount() == 12) {
            state_ = PersistentState::Decode(buf);
        }
    }

    void PersistState() {
        std::string path = log_dir_ + "/raft_state";
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        std::string data = state_.Encode();
        f.write(data.data(), data.size());
        f.flush();
    }

    void LoadEntries() {
        std::string path = log_dir_ + "/raft_log";
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return;
        while (f.good() && !f.eof()) {
            uint32_t entry_len = 0;
            f.read(reinterpret_cast<char*>(&entry_len), 4);
            if (!f.good() || entry_len == 0) break;
            std::string buf(entry_len, '\0');
            f.read(&buf[0], entry_len);
            if (!f.good()) break;
            entries_.push_back(LogEntry::Decode(buf.data(), buf.size()));
        }
    }

    void PersistEntry(const LogEntry& e) {
        std::string path = log_dir_ + "/raft_log";
        std::ofstream f(path, std::ios::binary | std::ios::app);
        std::string data = e.Encode();
        uint32_t len = static_cast<uint32_t>(data.size());
        f.write(reinterpret_cast<const char*>(&len), 4);
        f.write(data.data(), data.size());
        f.flush();
    }

    std::string               log_dir_;
    mutable absl::Mutex       mu_;
    std::deque<LogEntry>      entries_ ABSL_GUARDED_BY(mu_);
    uint64_t                  first_index_;
    PersistentState           state_ ABSL_GUARDED_BY(mu_);
    std::atomic<uint64_t>     last_applied_;
};

}  // namespace raft
}  // namespace ai_kv
