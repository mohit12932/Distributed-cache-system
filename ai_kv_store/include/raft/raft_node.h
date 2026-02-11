// ai_kv_store/include/raft/raft_node.h
// ────────────────────────────────────────────────────────────────
// Raft consensus node: Leader Election + Log Replication for a
// 3-node cluster. Implements the core Raft state machine with
// randomized election timeouts and pipelined AppendEntries.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "absl/synchronization/mutex.h"
#include "absl/synchronization/notification.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "raft_log.h"

namespace ai_kv {
namespace raft {

// ── Cluster configuration ──

static constexpr int kClusterSize = 3;
static constexpr int kMajority    = 2;

struct PeerInfo {
    uint32_t    id;
    std::string address;   // "host:port"
};

// ── Raft RPC request/response types (C++ mirrors of protobuf) ──

struct AppendEntriesReq {
    uint64_t              term;
    uint32_t              leader_id;
    uint64_t              prev_log_index;
    uint64_t              prev_log_term;
    std::vector<LogEntry> entries;
    uint64_t              leader_commit;
};

struct AppendEntriesResp {
    uint64_t term;
    bool     success;
    uint64_t match_index;
    uint64_t conflict_index;
    uint64_t conflict_term;
};

struct RequestVoteReq {
    uint64_t term;
    uint32_t candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
};

struct RequestVoteResp {
    uint64_t term;
    bool     vote_granted;
};

// ── Transport interface (implemented by gRPC layer) ──

class RaftTransport {
public:
    virtual ~RaftTransport() = default;
    virtual AppendEntriesResp SendAppendEntries(uint32_t peer_id,
                                                const AppendEntriesReq& req) = 0;
    virtual RequestVoteResp   SendRequestVote(uint32_t peer_id,
                                              const RequestVoteReq& req)     = 0;
};

// ── Apply callback: invoked when a log entry is committed ──

using ApplyCallback = std::function<void(uint64_t index, const LogEntry& entry)>;

// ── Node role ──

enum class Role { kFollower, kCandidate, kLeader };

inline const char* RoleName(Role r) {
    switch (r) {
        case Role::kFollower:  return "Follower";
        case Role::kCandidate: return "Candidate";
        case Role::kLeader:    return "Leader";
    }
    return "Unknown";
}

// ── Leader-specific volatile state ──

struct LeaderState {
    // Per-peer tracking
    std::unordered_map<uint32_t, uint64_t> next_index;   // Next entry to send
    std::unordered_map<uint32_t, uint64_t> match_index;  // Highest replicated

    void Reset(const std::vector<PeerInfo>& peers, uint64_t last_log_index) {
        next_index.clear();
        match_index.clear();
        for (const auto& p : peers) {
            next_index[p.id]  = last_log_index + 1;
            match_index[p.id] = 0;
        }
    }
};

// ═══════════════════════════════════════════════════════════════════
//  RaftNode — Core State Machine
// ═══════════════════════════════════════════════════════════════════

class RaftNode {
public:
    RaftNode(uint32_t id,
             const std::vector<PeerInfo>& peers,
             const std::string& log_dir,
             std::shared_ptr<RaftTransport> transport,
             ApplyCallback apply_cb)
        : id_(id),
          peers_(peers),
          log_(log_dir),
          transport_(std::move(transport)),
          apply_cb_(std::move(apply_cb)),
          role_(Role::kFollower),
          commit_index_(0),
          shutdown_(false) {
        // Load persisted term/votedFor
        auto state = log_.GetState();
        current_term_ = state.current_term;
        voted_for_    = state.voted_for;

        ResetElectionTimer();
    }

    ~RaftNode() { Shutdown(); }

    // ── Lifecycle ──

    void Start() {
        ticker_thread_ = std::thread([this] { TickerLoop(); });
        applier_thread_ = std::thread([this] { ApplierLoop(); });
    }

    void Shutdown() {
        shutdown_.store(true, std::memory_order_release);
        if (ticker_thread_.joinable()) ticker_thread_.join();
        if (applier_thread_.joinable()) applier_thread_.join();
    }

    // ── Client-facing: propose a new command ──

    struct ProposeResult {
        bool     accepted;       // True if this node is leader
        uint64_t index;          // Log index if accepted
        uint64_t term;
        std::string leader_hint; // Address of known leader
    };

    ProposeResult Propose(const std::string& command) {
        absl::MutexLock lock(&mu_);

        if (role_ != Role::kLeader) {
            std::string hint;
            for (const auto& p : peers_) {
                if (p.id == static_cast<uint32_t>(leader_id_)) {
                    hint = p.address;
                    break;
                }
            }
            return {false, 0, current_term_, hint};
        }

        // Append to leader's log
        uint64_t new_index = log_.LastIndex() + 1;
        LogEntry entry{current_term_, new_index, EntryType::kNormal, command};
        log_.Append(entry);

        // Trigger immediate replication (normally done by heartbeat ticker)
        // The ticker loop will pick this up on next iteration.

        return {true, new_index, current_term_, ""};
    }

    // ── RPC Handlers (called by gRPC service implementation) ──

    AppendEntriesResp HandleAppendEntries(const AppendEntriesReq& req) {
        absl::MutexLock lock(&mu_);
        AppendEntriesResp resp;
        resp.term    = current_term_;
        resp.success = false;

        // Rule 1: Reply false if term < currentTerm
        if (req.term < current_term_) return resp;

        // Recognize authority
        if (req.term > current_term_) {
            StepDown(req.term);
        }
        role_ = Role::kFollower;
        leader_id_ = req.leader_id;
        ResetElectionTimer();

        // Rule 2: Reply false if log doesn't contain prev entry
        if (req.prev_log_index > 0) {
            uint64_t local_term = log_.TermAt(req.prev_log_index);
            if (local_term == 0 && req.prev_log_index > log_.LastIndex()) {
                // Missing entry
                resp.conflict_index = log_.LastIndex() + 1;
                resp.conflict_term  = 0;
                return resp;
            }
            if (local_term != req.prev_log_term) {
                // Term mismatch — find first entry with this conflicting term
                resp.conflict_term  = local_term;
                resp.conflict_index = req.prev_log_index;
                // Walk back to find start of conflicting term
                while (resp.conflict_index > log_.FirstIndex() &&
                       log_.TermAt(resp.conflict_index - 1) == local_term) {
                    --resp.conflict_index;
                }
                return resp;
            }
        }

        // Rule 3: Append new entries, truncating conflicts
        uint64_t insert_index = req.prev_log_index + 1;
        for (size_t i = 0; i < req.entries.size(); ++i) {
            uint64_t idx = insert_index + i;
            uint64_t existing_term = log_.TermAt(idx);
            if (existing_term != 0 && existing_term != req.entries[i].term) {
                log_.TruncateFrom(idx);
            }
            if (idx > log_.LastIndex()) {
                log_.Append(req.entries[i]);
            }
        }

        // Rule 4: Update commit index
        if (req.leader_commit > commit_index_) {
            commit_index_ = std::min(req.leader_commit, log_.LastIndex());
        }

        resp.success     = true;
        resp.match_index = log_.LastIndex();
        resp.term        = current_term_;
        return resp;
    }

    RequestVoteResp HandleRequestVote(const RequestVoteReq& req) {
        absl::MutexLock lock(&mu_);
        RequestVoteResp resp;
        resp.term         = current_term_;
        resp.vote_granted = false;

        if (req.term < current_term_) return resp;

        if (req.term > current_term_) {
            StepDown(req.term);
        }

        // Grant vote if we haven't voted for someone else in this term,
        // AND the candidate's log is at least as up-to-date
        bool can_vote = (voted_for_ == -1 ||
                         voted_for_ == static_cast<int32_t>(req.candidate_id));

        bool log_ok = (req.last_log_term > log_.LastTerm()) ||
                      (req.last_log_term == log_.LastTerm() &&
                       req.last_log_index >= log_.LastIndex());

        if (can_vote && log_ok) {
            voted_for_ = req.candidate_id;
            PersistVotedFor();
            ResetElectionTimer();
            resp.vote_granted = true;
        }

        resp.term = current_term_;
        return resp;
    }

    // ── Introspection ──

    Role    GetRole()    const { absl::ReaderMutexLock l(&mu_); return role_; }
    uint64_t GetTerm()   const { absl::ReaderMutexLock l(&mu_); return current_term_; }
    uint32_t GetId()     const { return id_; }
    bool     IsLeader()  const { return GetRole() == Role::kLeader; }

private:
    // ── Background ticker: drives elections and heartbeats ──

    void TickerLoop() {
        while (!shutdown_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            absl::MutexLock lock(&mu_);
            auto now = std::chrono::steady_clock::now();

            if (role_ == Role::kLeader) {
                // Send heartbeats / replicate entries every 50ms
                if (now >= next_heartbeat_) {
                    SendHeartbeats();
                    next_heartbeat_ = now + std::chrono::milliseconds(50);
                }
            } else {
                // Check election timeout
                if (now >= election_deadline_) {
                    StartElection();
                }
            }
        }
    }

    // ── Election ──

    void StartElection() {
        ++current_term_;
        role_     = Role::kCandidate;
        voted_for_ = id_;
        PersistTermAndVote();
        ResetElectionTimer();

        int votes_received = 1;  // Vote for self

        RequestVoteReq req;
        req.term           = current_term_;
        req.candidate_id   = id_;
        req.last_log_index = log_.LastIndex();
        req.last_log_term  = log_.LastTerm();

        // Send RequestVote to all peers (could be parallelized with futures)
        for (const auto& peer : peers_) {
            if (peer.id == id_) continue;

            // Release lock during RPC
            mu_.Unlock();
            auto resp = transport_->SendRequestVote(peer.id, req);
            mu_.Lock();

            // Check if we're still a candidate (might have stepped down)
            if (role_ != Role::kCandidate || current_term_ != req.term) return;

            if (resp.term > current_term_) {
                StepDown(resp.term);
                return;
            }

            if (resp.vote_granted) {
                ++votes_received;
                if (votes_received >= kMajority) {
                    BecomeLeader();
                    return;
                }
            }
        }
    }

    void BecomeLeader() {
        role_      = Role::kLeader;
        leader_id_ = id_;
        leader_state_.Reset(peers_, log_.LastIndex());

        // Append a no-op entry to commit entries from prior terms
        LogEntry noop{current_term_, log_.LastIndex() + 1, EntryType::kNoop, ""};
        log_.Append(noop);

        SendHeartbeats();
        next_heartbeat_ = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(50);
    }

    // ── Replication ──

    void SendHeartbeats() {
        for (const auto& peer : peers_) {
            if (peer.id == id_) continue;
            ReplicateTo(peer.id);
        }
        AdvanceCommitIndex();
    }

    void ReplicateTo(uint32_t peer_id) {
        uint64_t next_idx = leader_state_.next_index[peer_id];
        uint64_t prev_idx = next_idx - 1;

        AppendEntriesReq req;
        req.term           = current_term_;
        req.leader_id      = id_;
        req.prev_log_index = prev_idx;
        req.prev_log_term  = log_.TermAt(prev_idx);
        req.leader_commit  = commit_index_;

        // Batch entries from next_index to last_index
        uint64_t last = log_.LastIndex();
        if (next_idx <= last) {
            req.entries = log_.Slice(next_idx, std::min(last, next_idx + 99));
        }

        // Send (release lock during network call)
        mu_.Unlock();
        auto resp = transport_->SendAppendEntries(peer_id, req);
        mu_.Lock();

        // Validate we're still leader in the same term
        if (role_ != Role::kLeader || current_term_ != req.term) return;

        if (resp.term > current_term_) {
            StepDown(resp.term);
            return;
        }

        if (resp.success) {
            leader_state_.match_index[peer_id] = resp.match_index;
            leader_state_.next_index[peer_id]  = resp.match_index + 1;
        } else {
            // Decrement next_index using conflict hint for fast catch-up
            if (resp.conflict_term > 0) {
                // Search for last entry with conflict_term
                leader_state_.next_index[peer_id] = resp.conflict_index;
            } else {
                leader_state_.next_index[peer_id] =
                    std::max(uint64_t(1), resp.conflict_index);
            }
        }
    }

    void AdvanceCommitIndex() {
        // Find the highest index replicated on a majority
        for (uint64_t n = log_.LastIndex(); n > commit_index_; --n) {
            if (log_.TermAt(n) != current_term_) continue;

            int replication_count = 1;  // Self
            for (const auto& peer : peers_) {
                if (peer.id == id_) continue;
                if (leader_state_.match_index[peer.id] >= n) {
                    ++replication_count;
                }
            }
            if (replication_count >= kMajority) {
                commit_index_ = n;
                break;
            }
        }
    }

    // ── Applier: applies committed entries to the state machine ──

    void ApplierLoop() {
        while (!shutdown_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));

            uint64_t last_applied = log_.LastApplied();
            uint64_t commit = commit_index_.load(std::memory_order_acquire);

            while (last_applied < commit) {
                ++last_applied;
                const LogEntry* entry = log_.Entry(last_applied);
                if (entry && apply_cb_) {
                    apply_cb_(last_applied, *entry);
                }
                log_.SetLastApplied(last_applied);
            }
        }
    }

    // ── Helpers ──

    void StepDown(uint64_t new_term) {
        current_term_ = new_term;
        role_          = Role::kFollower;
        voted_for_     = -1;
        PersistTermAndVote();
        ResetElectionTimer();
    }

    void ResetElectionTimer() {
        static thread_local std::mt19937 rng(std::random_device{}());
        std::uniform_int_distribution<int> dist(150, 300);
        election_deadline_ = std::chrono::steady_clock::now() +
                             std::chrono::milliseconds(dist(rng));
    }

    void PersistTermAndVote() {
        PersistentState s{current_term_, voted_for_};
        log_.SetState(s);
    }

    void PersistVotedFor() {
        PersistTermAndVote();
    }

    // ── Fields ──

    const uint32_t              id_;
    std::vector<PeerInfo>       peers_;
    RaftLog                     log_;
    std::shared_ptr<RaftTransport> transport_;
    ApplyCallback               apply_cb_;

    mutable absl::Mutex         mu_;
    Role                        role_          ABSL_GUARDED_BY(mu_) = Role::kFollower;
    uint64_t                    current_term_  ABSL_GUARDED_BY(mu_) = 0;
    int32_t                     voted_for_     ABSL_GUARDED_BY(mu_) = -1;
    int32_t                     leader_id_     ABSL_GUARDED_BY(mu_) = -1;
    std::atomic<uint64_t>       commit_index_;
    LeaderState                 leader_state_  ABSL_GUARDED_BY(mu_);

    // Timing
    std::chrono::steady_clock::time_point election_deadline_ ABSL_GUARDED_BY(mu_);
    std::chrono::steady_clock::time_point next_heartbeat_    ABSL_GUARDED_BY(mu_);

    // Background threads
    std::thread     ticker_thread_;
    std::thread     applier_thread_;
    std::atomic<bool> shutdown_;
};

}  // namespace raft
}  // namespace ai_kv
