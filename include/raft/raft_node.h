#pragma once
// ────────────────────────────────────────────────────────────────
// Raft Node: Full Raft consensus implementation with leader
// election, log replication, and state machine application.
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../compat/threading.h"
#include "raft_log.h"

namespace dcs {
namespace raft {

enum class RaftRole { Follower, Candidate, Leader };

inline const char* RoleToString(RaftRole r) {
    switch (r) {
        case RaftRole::Follower:  return "Follower";
        case RaftRole::Candidate: return "Candidate";
        case RaftRole::Leader:    return "Leader";
    }
    return "Unknown";
}

// ──── RPC Messages ─────────────────────────────────────────────

struct RequestVoteArgs {
    uint64_t term;
    int      candidate_id;
    uint64_t last_log_index;
    uint64_t last_log_term;
};

struct RequestVoteReply {
    uint64_t term;
    bool     vote_granted;
};

struct AppendEntriesArgs {
    uint64_t              term;
    int                   leader_id;
    uint64_t              prev_log_index;
    uint64_t              prev_log_term;
    std::vector<LogEntry> entries;
    uint64_t              leader_commit;
};

struct AppendEntriesReply {
    uint64_t term;
    bool     success;
    uint64_t match_index;
};

// ──── Transport Interface ──────────────────────────────────────

class RaftTransport {
public:
    virtual ~RaftTransport() = default;
    virtual RequestVoteReply    SendRequestVote(int peer_id, const RequestVoteArgs& args)    = 0;
    virtual AppendEntriesReply SendAppendEntries(int peer_id, const AppendEntriesArgs& args) = 0;
};

// ──── Raft Node ────────────────────────────────────────────────

class RaftNode {
public:
    using ApplyCallback = std::function<void(uint64_t index, const std::string& command)>;

    RaftNode(int node_id, int cluster_size, const std::string& data_dir = "data/raft")
        : id_(node_id), cluster_size_(cluster_size),
          role_(RaftRole::Follower), commit_index_(0), last_applied_(0),
          running_(false), log_(data_dir),
          election_timeout_ms_(150 + (node_id * 50) % 150),
          rng_(static_cast<unsigned>(
              std::chrono::steady_clock::now().time_since_epoch().count())) {
        next_index_.resize(cluster_size_, 1);
        match_index_.resize(cluster_size_, 0);
    }

    ~RaftNode() { Stop(); }

    void SetTransport(RaftTransport* transport) { transport_ = transport; }
    void SetApplyCallback(ApplyCallback cb) { apply_cb_ = std::move(cb); }

    void Start() {
        running_ = true;
        ResetElectionTimer();
        ticker_thread_ = compat::Thread(&RaftNode::TickerLoop, this);
        applier_thread_ = compat::Thread(&RaftNode::ApplierLoop, this);
    }

    void Stop() {
        running_ = false;
        if (ticker_thread_.joinable()) ticker_thread_.join();
        if (applier_thread_.joinable()) applier_thread_.join();
    }

    // Force a leadership election (for demo purposes)
    void TriggerElection() {
        StartElection();
    }

    // ─── Client Interface ──────────────────────────────────────

    // Propose a new command (only leader can accept)
    bool Propose(const std::string& command) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        if (role_ != RaftRole::Leader) return false;

        LogEntry entry;
        entry.term    = log_.CurrentTerm();
        entry.index   = log_.LastIndex() + 1;
        entry.command = command;
        log_.Append(entry);
        // Will be replicated on next heartbeat
        return true;
    }

    bool IsLeader() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        return role_ == RaftRole::Leader;
    }

    // ─── RPC Handlers ──────────────────────────────────────────

    RequestVoteReply HandleRequestVote(const RequestVoteArgs& args) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        RequestVoteReply reply;
        reply.term = log_.CurrentTerm();
        reply.vote_granted = false;

        if (args.term < log_.CurrentTerm()) return reply;

        if (args.term > log_.CurrentTerm()) {
            BecomeFollower(args.term);
        }

        int voted_for = log_.VotedFor();
        bool log_ok = (args.last_log_term > log_.LastTerm()) ||
                      (args.last_log_term == log_.LastTerm() &&
                       args.last_log_index >= log_.LastIndex());

        if ((voted_for == -1 || voted_for == args.candidate_id) && log_ok) {
            log_.SetVotedFor(args.candidate_id);
            reply.vote_granted = true;
            ResetElectionTimer();
        }

        reply.term = log_.CurrentTerm();
        return reply;
    }

    AppendEntriesReply HandleAppendEntries(const AppendEntriesArgs& args) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        AppendEntriesReply reply;
        reply.term = log_.CurrentTerm();
        reply.success = false;
        reply.match_index = 0;

        if (args.term < log_.CurrentTerm()) return reply;

        if (args.term >= log_.CurrentTerm()) {
            BecomeFollower(args.term);
        }
        ResetElectionTimer();
        leader_id_ = args.leader_id;

        // Check log consistency
        if (args.prev_log_index > 0 &&
            !log_.MatchesAt(args.prev_log_index, args.prev_log_term)) {
            return reply;
        }

        // Append new entries
        if (!args.entries.empty()) {
            // Check for conflicts and truncate
            for (const auto& entry : args.entries) {
                uint64_t existing_term = log_.TermAt(entry.index);
                if (existing_term != 0 && existing_term != entry.term) {
                    log_.TruncateFrom(entry.index);
                    break;
                }
            }
            // Append entries that are not yet in the log
            for (const auto& entry : args.entries) {
                if (entry.index > log_.LastIndex()) {
                    log_.Append(entry);
                }
            }
        }

        // Update commit index
        if (args.leader_commit > commit_index_) {
            commit_index_ = std::min(args.leader_commit, log_.LastIndex());
        }

        reply.success = true;
        reply.match_index = log_.LastIndex();
        reply.term = log_.CurrentTerm();
        return reply;
    }

    // ─── State Queries ─────────────────────────────────────────

    struct NodeState {
        int         id;
        RaftRole    role;
        uint64_t    term;
        uint64_t    commit_index;
        uint64_t    last_applied;
        uint64_t    log_size;
        int         leader_id;
        int         votes_received;
    };

    NodeState GetState() const {
        compat::LockGuard<compat::Mutex> lock(const_cast<compat::Mutex&>(mu_));
        return {id_, role_, log_.CurrentTerm(), commit_index_,
                last_applied_, log_.Size(), leader_id_, votes_received_};
    }

private:
    void TickerLoop() {
        while (running_) {
            compat::this_thread::sleep_for(std::chrono::milliseconds(50));
            int action = 0; // 0=none, 1=heartbeat, 2=election
            {
                compat::LockGuard<compat::Mutex> lock(mu_);
                auto now = std::chrono::steady_clock::now();
                if (role_ == RaftRole::Leader) {
                    action = 1;
                } else {
                    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - last_heartbeat_).count();
                    if (elapsed >= election_timeout_ms_) {
                        action = 2;
                    }
                }
            }
            // RPC calls made WITHOUT holding mu_ to avoid deadlocks
            if (action == 1) SendHeartbeats();
            else if (action == 2) StartElection();
        }
    }

    void ApplierLoop() {
        while (running_) {
            compat::this_thread::sleep_for(std::chrono::milliseconds(10));
            compat::LockGuard<compat::Mutex> lock(mu_);
            while (last_applied_ < commit_index_) {
                last_applied_++;
                LogEntry entry;
                if (log_.GetEntry(last_applied_, entry) && apply_cb_) {
                    apply_cb_(entry.index, entry.command);
                }
            }
        }
    }

    void StartElection() {
        // Prepare election state under lock, then release before RPCs
        RequestVoteArgs args;
        int majority;
        int my_id;
        int cs;
        {
            compat::LockGuard<compat::Mutex> lock(mu_);
            uint64_t new_term = log_.CurrentTerm() + 1;
            log_.SetTerm(new_term);
            log_.SetVotedFor(id_);
            role_ = RaftRole::Candidate;
            votes_received_ = 1;  // vote for self
            ResetElectionTimer();

            if (!transport_) return;

            args.term           = new_term;
            args.candidate_id   = id_;
            args.last_log_index = log_.LastIndex();
            args.last_log_term  = log_.LastTerm();
            majority = (cluster_size_ / 2) + 1;
            my_id = id_;
            cs = cluster_size_;
        }
        // RPCs made without holding mu_ to avoid deadlocks
        for (int peer = 0; peer < cs; peer++) {
            if (peer == my_id) continue;
            try {
                auto reply = transport_->SendRequestVote(peer, args);
                compat::LockGuard<compat::Mutex> lock(mu_);
                if (role_ != RaftRole::Candidate) return;
                if (reply.term > log_.CurrentTerm()) {
                    BecomeFollower(reply.term);
                    return;
                }
                if (reply.vote_granted) {
                    votes_received_++;
                    if (votes_received_ >= majority) {
                        BecomeLeader();
                        return;
                    }
                }
            } catch (...) {
                // Peer unreachable — continue
            }
        }
    }

    void SendHeartbeats() {
        // Prepare heartbeat args under lock, then send without lock
        struct HBInfo { int peer; AppendEntriesArgs args; };
        std::vector<HBInfo> hbs;
        {
            compat::LockGuard<compat::Mutex> lock(mu_);
            if (!transport_ || role_ != RaftRole::Leader) return;
            for (int peer = 0; peer < cluster_size_; peer++) {
                if (peer == id_) continue;
                AppendEntriesArgs args;
                args.term           = log_.CurrentTerm();
                args.leader_id      = id_;
                args.leader_commit  = commit_index_;
                uint64_t next = next_index_[peer];
                args.prev_log_index = (next > 1) ? next - 1 : 0;
                args.prev_log_term  = log_.TermAt(args.prev_log_index);
                if (log_.LastIndex() >= next) {
                    args.entries = log_.GetRange(next);
                }
                hbs.push_back({peer, std::move(args)});
            }
        }
        // RPCs made without holding mu_
        for (auto& hb : hbs) {
            try {
                auto reply = transport_->SendAppendEntries(hb.peer, hb.args);
                compat::LockGuard<compat::Mutex> lock(mu_);
                if (role_ != RaftRole::Leader) return;
                if (reply.term > log_.CurrentTerm()) {
                    BecomeFollower(reply.term);
                    return;
                }
                if (reply.success) {
                    match_index_[hb.peer] = reply.match_index;
                    next_index_[hb.peer]  = reply.match_index + 1;
                    TryAdvanceCommit();
                } else {
                    if (next_index_[hb.peer] > 1) next_index_[hb.peer]--;
                }
            } catch (...) {
                // Peer unreachable
            }
        }
    }

    void TryAdvanceCommit() {
        // Find the highest N such that a majority of match_index[i] >= N
        for (uint64_t n = log_.LastIndex(); n > commit_index_; n--) {
            if (log_.TermAt(n) != log_.CurrentTerm()) continue;
            int count = 1;  // self
            for (int i = 0; i < cluster_size_; i++) {
                if (i != id_ && match_index_[i] >= n) count++;
            }
            if (count > cluster_size_ / 2) {
                commit_index_ = n;
                break;
            }
        }
    }

    void BecomeFollower(uint64_t term) {
        role_ = RaftRole::Follower;
        log_.SetTerm(term);
        votes_received_ = 0;
        ResetElectionTimer();
    }

    void BecomeLeader() {
        role_ = RaftRole::Leader;
        leader_id_ = id_;
        for (int i = 0; i < cluster_size_; i++) {
            next_index_[i]  = log_.LastIndex() + 1;
            match_index_[i] = 0;
        }
        // Immediately send heartbeats
        SendHeartbeats();
    }

    void ResetElectionTimer() {
        last_heartbeat_ = std::chrono::steady_clock::now();
        // Randomize timeout: 150-300ms
        std::uniform_int_distribution<int> dist(150, 300);
        election_timeout_ms_ = dist(rng_);
    }

    int         id_;
    int         cluster_size_;
    RaftRole    role_;
    uint64_t    commit_index_;
    uint64_t    last_applied_;
    int         leader_id_ = -1;
    int         votes_received_ = 0;

    bool        running_;
    RaftLog     log_;

    int         election_timeout_ms_;
    std::chrono::steady_clock::time_point last_heartbeat_;
    std::mt19937 rng_;

    std::vector<uint64_t> next_index_;
    std::vector<uint64_t> match_index_;

    RaftTransport* transport_ = nullptr;
    ApplyCallback  apply_cb_;

    compat::Mutex mu_;
    compat::Thread   ticker_thread_;
    compat::Thread   applier_thread_;
};

// ──── Local Transport (for single-process simulation) ──────────

class LocalRaftTransport : public RaftTransport {
public:
    void RegisterNode(int id, RaftNode* node) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        nodes_[id] = node;
    }

    RequestVoteReply SendRequestVote(int peer_id, const RequestVoteArgs& args) override {
        RaftNode* node = nullptr;
        {
            compat::LockGuard<compat::Mutex> lock(mu_);
            auto it = nodes_.find(peer_id);
            if (it == nodes_.end()) return {args.term, false};
            node = it->second;
        }
        return node->HandleRequestVote(args);
    }

    AppendEntriesReply SendAppendEntries(int peer_id, const AppendEntriesArgs& args) override {
        RaftNode* node = nullptr;
        {
            compat::LockGuard<compat::Mutex> lock(mu_);
            auto it = nodes_.find(peer_id);
            if (it == nodes_.end()) return {args.term, false, 0};
            node = it->second;
        }
        return node->HandleAppendEntries(args);
    }

private:
    std::unordered_map<int, RaftNode*> nodes_;
    compat::Mutex mu_;
};

}  // namespace raft
}  // namespace dcs
