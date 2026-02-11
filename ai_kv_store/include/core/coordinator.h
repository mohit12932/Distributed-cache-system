// ai_kv_store/include/core/coordinator.h
// ────────────────────────────────────────────────────────────────
// Top-level Coordinator: wires together LSM-Tree, Raft, PINN,
// and Shard Manager into a single node process.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <string>

#include "absl/strings/string_view.h"
#include "absl/synchronization/mutex.h"

#include "../storage/lsm_tree.h"
#include "../raft/raft_node.h"
#include "../ml/predictive_sharder.h"
#include "shard_manager.h"

namespace ai_kv {
namespace core {

// ── Node configuration ──

struct NodeConfig {
    uint32_t                      node_id;
    std::string                   address;          // "host:port"
    std::vector<raft::PeerInfo>   peers;
    std::string                   data_dir     = "./data";
    int                           num_shards   = 8;
    double                        pressure_threshold = 0.8;
    size_t                        memtable_size = 4 * 1024 * 1024;
};

// ── Coordinator ──

class Coordinator {
public:
    explicit Coordinator(const NodeConfig& config)
        : config_(config), shutdown_(false) {

        // ── Initialize storage engine ──
        storage::LSMConfig lsm_cfg;
        lsm_cfg.data_dir       = config.data_dir + "/lsm";
        lsm_cfg.memtable_size  = config.memtable_size;
        lsm_tree_ = std::make_unique<storage::LSMTree>(lsm_cfg);

        // ── Initialize shard manager ──
        shard_ring_ = std::make_unique<ConsistentHashRing>();
        for (int s = 0; s < config.num_shards; ++s) {
            ShardInfo info;
            info.id           = s;
            info.node_id      = config.node_id;
            info.node_address = config.address;
            info.key_count    = 0;
            info.byte_size    = 0;
            info.migrating    = false;
            shard_ring_->AddShard(info);
        }

        // ── Initialize predictive sharder ──
        ml::PredictiveSharder::Config sharder_cfg;
        sharder_cfg.num_shards         = config.num_shards;
        sharder_cfg.pressure_threshold = config.pressure_threshold;
        sharder_ = std::make_unique<ml::PredictiveSharder>(sharder_cfg);

        sharder_->SetMigrationCallback(
            [this](const ml::MigrationRequest& req) {
                HandleMigrationRequest(req);
            });

        // ── Initialize Raft (transport provided externally) ──
        // RaftNode is created after transport is set via SetTransport()
    }

    ~Coordinator() { Shutdown(); }

    // ── Lifecycle ──

    void SetTransport(std::shared_ptr<raft::RaftTransport> transport) {
        raft_node_ = std::make_unique<raft::RaftNode>(
            config_.node_id,
            config_.peers,
            config_.data_dir + "/raft",
            transport,
            [this](uint64_t index, const raft::LogEntry& entry) {
                ApplyCommitted(index, entry);
            });
    }

    void Start() {
        if (raft_node_)  raft_node_->Start();
        if (sharder_)    sharder_->Start();
        std::cout << "[Coordinator] Node " << config_.node_id
                  << " started on " << config_.address << "\n";
    }

    void Shutdown() {
        if (shutdown_.exchange(true)) return;
        if (sharder_)    sharder_->Stop();
        if (raft_node_)  raft_node_->Shutdown();
        std::cout << "[Coordinator] Node " << config_.node_id << " shut down\n";
    }

    // ── Client operations (routed through Raft if leader) ──

    struct OpResult {
        bool        success;
        std::string value;
        std::string error;
        std::string redirect;  // Leader address if not leader
    };

    OpResult Get(absl::string_view key) {
        // Route to correct shard/node
        uint32_t shard = shard_ring_->GetShard(key);
        // For simplicity, assume local shard ownership check
        // In production: forward to owning node via gRPC

        // Record telemetry
        auto start = std::chrono::steady_clock::now();

        auto result = lsm_tree_->Get(key);

        auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - start).count();
        sharder_->RecordOperation(shard, elapsed, false);

        if (result.found) {
            return {true, result.value, "", ""};
        }
        return {false, "", "key not found", ""};
    }

    OpResult Put(absl::string_view key, absl::string_view value) {
        if (!raft_node_ || !raft_node_->IsLeader()) {
            // Forward to leader
            std::string leader;
            if (raft_node_) {
                auto prop = raft_node_->Propose("");
                leader = prop.leader_hint;
            }
            return {false, "", "not leader", leader};
        }

        // Serialize command and propose through Raft
        std::string command = SerializePut(key, value);
        auto result = raft_node_->Propose(command);

        if (!result.accepted) {
            return {false, "", "proposal rejected", result.leader_hint};
        }

        // Record telemetry (write)
        uint32_t shard = shard_ring_->GetShard(key);
        sharder_->RecordOperation(shard, 0, true);

        return {true, "", "", ""};
    }

    OpResult Delete(absl::string_view key) {
        if (!raft_node_ || !raft_node_->IsLeader()) {
            return {false, "", "not leader", ""};
        }

        std::string command = SerializeDelete(key);
        auto result = raft_node_->Propose(command);
        return {result.accepted, "", result.accepted ? "" : "rejected", ""};
    }

    // ── Introspection ──

    bool    IsLeader()  const { return raft_node_ && raft_node_->IsLeader(); }
    uint32_t NodeId()   const { return config_.node_id; }

    storage::LSMTree::Stats GetStorageStats() const {
        return lsm_tree_->GetStats();
    }

    std::vector<ml::PINNModel::ShardPrediction> GetHeatMap() const {
        return sharder_->GetCurrentHeatMap();
    }

    // ── Raft RPC handlers (called by gRPC service) ──

    raft::AppendEntriesResp HandleAppendEntries(const raft::AppendEntriesReq& req) {
        return raft_node_->HandleAppendEntries(req);
    }

    raft::RequestVoteResp HandleRequestVote(const raft::RequestVoteReq& req) {
        return raft_node_->HandleRequestVote(req);
    }

private:
    // ── Apply a committed Raft log entry to the state machine (LSM-Tree) ──

    void ApplyCommitted(uint64_t /*index*/, const raft::LogEntry& entry) {
        if (entry.type == raft::EntryType::kNoop) return;
        if (entry.type == raft::EntryType::kShardMove) {
            // Handle shard migration command
            return;
        }

        // Deserialize and apply KV operation
        if (entry.command.size() < 2) return;
        uint8_t op = static_cast<uint8_t>(entry.command[0]);

        if (op == 0x01) {  // PUT
            auto [key, value] = DeserializePut(entry.command);
            lsm_tree_->Put(key, value);
        } else if (op == 0x02) {  // DELETE
            auto key = DeserializeDelete(entry.command);
            lsm_tree_->Delete(key);
        }
    }

    // ── Handle PINN-triggered migration ──

    void HandleMigrationRequest(const ml::MigrationRequest& req) {
        std::cout << "[Migration] PINN predicted pressure "
                  << req.predicted_heat_source << " on shard "
                  << req.source_shard << " → migrating to shard "
                  << req.target_shard << " (heat: "
                  << req.predicted_heat_target << ")\n";

        // In production:
        // 1. Propose ShardMove entry through Raft for consensus
        // 2. On commit: initiate background key range transfer via gRPC streaming
        // 3. Update consistent hash ring atomically
        // 4. Notify all nodes of routing change
    }

    // ── Serialization helpers ──

    static std::string SerializePut(absl::string_view key, absl::string_view value) {
        std::string cmd;
        cmd.push_back(0x01);  // PUT opcode
        uint32_t klen = static_cast<uint32_t>(key.size());
        uint32_t vlen = static_cast<uint32_t>(value.size());
        cmd.append(reinterpret_cast<const char*>(&klen), 4);
        cmd.append(key.data(), key.size());
        cmd.append(reinterpret_cast<const char*>(&vlen), 4);
        cmd.append(value.data(), value.size());
        return cmd;
    }

    static std::pair<std::string, std::string> DeserializePut(const std::string& cmd) {
        size_t pos = 1;
        uint32_t klen = 0;
        std::memcpy(&klen, cmd.data() + pos, 4); pos += 4;
        std::string key = cmd.substr(pos, klen); pos += klen;
        uint32_t vlen = 0;
        std::memcpy(&vlen, cmd.data() + pos, 4); pos += 4;
        std::string value = cmd.substr(pos, vlen);
        return {key, value};
    }

    static std::string SerializeDelete(absl::string_view key) {
        std::string cmd;
        cmd.push_back(0x02);  // DELETE opcode
        uint32_t klen = static_cast<uint32_t>(key.size());
        cmd.append(reinterpret_cast<const char*>(&klen), 4);
        cmd.append(key.data(), key.size());
        return cmd;
    }

    static std::string DeserializeDelete(const std::string& cmd) {
        uint32_t klen = 0;
        std::memcpy(&klen, cmd.data() + 1, 4);
        return cmd.substr(5, klen);
    }

    // ── Fields ──

    NodeConfig                                   config_;
    std::unique_ptr<storage::LSMTree>            lsm_tree_;
    std::unique_ptr<ConsistentHashRing>          shard_ring_;
    std::unique_ptr<raft::RaftNode>              raft_node_;
    std::unique_ptr<ml::PredictiveSharder>       sharder_;
    std::atomic<bool>                            shutdown_;
};

}  // namespace core
}  // namespace ai_kv
