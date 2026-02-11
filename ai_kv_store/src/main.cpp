// ai_kv_store/src/main.cpp
// ────────────────────────────────────────────────────────────────
// Entry point for the AI-Adaptive KV Store node.
// Parses CLI flags, initializes the Coordinator, and runs the
// gRPC server.
// ────────────────────────────────────────────────────────────────

#include <csignal>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_split.h"

#include "grpcpp/grpcpp.h"

#include "../include/core/coordinator.h"

// ── CLI Flags ──

ABSL_FLAG(uint32_t,    node_id,   0,       "Node ID (0, 1, or 2)");
ABSL_FLAG(std::string, address,   "",      "This node's listen address (host:port)");
ABSL_FLAG(std::string, peers,     "",      "Comma-separated peer addresses");
ABSL_FLAG(std::string, data_dir,  "./data","Data directory");
ABSL_FLAG(int,         shards,    8,       "Number of shards per node");
ABSL_FLAG(double,      threshold, 0.8,     "PINN pressure threshold for migration");
ABSL_FLAG(int,         memtable_mb, 4,     "MemTable size in MB");

// ── Shutdown signal handler ──

static std::atomic<bool> g_shutdown{false};

void SignalHandler(int /*sig*/) {
    g_shutdown.store(true, std::memory_order_release);
}

// ── Stub Raft transport (to be replaced with gRPC implementation) ──

class GrpcRaftTransport : public ai_kv::raft::RaftTransport {
public:
    explicit GrpcRaftTransport(
        const std::vector<ai_kv::raft::PeerInfo>& peers) : peers_(peers) {
        // In production: create gRPC stubs for each peer
        // for (const auto& peer : peers) {
        //     auto channel = grpc::CreateChannel(peer.address, grpc::InsecureChannelCredentials());
        //     stubs_[peer.id] = ai_kv::RaftService::NewStub(channel);
        // }
    }

    ai_kv::raft::AppendEntriesResp SendAppendEntries(
            uint32_t peer_id, const ai_kv::raft::AppendEntriesReq& req) override {
        // TODO: Serialize req → protobuf, send via gRPC stub, deserialize response
        // Placeholder: return failure (peer unreachable)
        return {req.term, false, 0, 0, 0};
    }

    ai_kv::raft::RequestVoteResp SendRequestVote(
            uint32_t peer_id, const ai_kv::raft::RequestVoteReq& req) override {
        // TODO: Serialize req → protobuf, send via gRPC stub, deserialize response
        return {req.term, false};
    }

private:
    std::vector<ai_kv::raft::PeerInfo> peers_;
    // std::unordered_map<uint32_t, std::unique_ptr<ai_kv::RaftService::Stub>> stubs_;
};

// ── gRPC Service Implementations ──

// TODO: Implement KVStoreServiceImpl, RaftServiceImpl, ShardMigrationServiceImpl
// These bridge gRPC requests to Coordinator methods:
//
// class KVStoreServiceImpl final : public ai_kv::KVStoreService::Service {
//     grpc::Status Get(grpc::ServerContext*, const ai_kv::GetRequest*,
//                      ai_kv::GetResponse*) override { ... }
//     grpc::Status Put(grpc::ServerContext*, const ai_kv::PutRequest*,
//                      ai_kv::PutResponse*) override { ... }
// };

// ── Main ──

int main(int argc, char** argv) {
    absl::ParseCommandLine(argc, argv);

    std::signal(SIGINT,  SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    // Parse configuration
    ai_kv::core::NodeConfig config;
    config.node_id           = absl::GetFlag(FLAGS_node_id);
    config.address           = absl::GetFlag(FLAGS_address);
    config.data_dir          = absl::GetFlag(FLAGS_data_dir);
    config.num_shards        = absl::GetFlag(FLAGS_shards);
    config.pressure_threshold = absl::GetFlag(FLAGS_threshold);
    config.memtable_size     = absl::GetFlag(FLAGS_memtable_mb) * 1024 * 1024;

    // Parse peer addresses
    std::string peers_str = absl::GetFlag(FLAGS_peers);
    std::vector<std::string> peer_addrs = absl::StrSplit(peers_str, ',');
    for (uint32_t i = 0; i < peer_addrs.size(); ++i) {
        config.peers.push_back({i, peer_addrs[i]});
    }

    if (config.address.empty() && config.node_id < peer_addrs.size()) {
        config.address = peer_addrs[config.node_id];
    }

    // ── Banner ──
    std::cout << R"(
    ┌────────────────────────────────────────────┐
    │  AI-Adaptive Distributed KV Store          │
    │  PINN-Guided Predictive Sharding           │
    └────────────────────────────────────────────┘
    )" << "\n";
    std::cout << "  Node ID:    " << config.node_id << "\n";
    std::cout << "  Address:    " << config.address << "\n";
    std::cout << "  Shards:     " << config.num_shards << "\n";
    std::cout << "  Threshold:  " << config.pressure_threshold << "\n";
    std::cout << "  Data Dir:   " << config.data_dir << "\n";
    std::cout << "  Peers:      " << peers_str << "\n\n";

    // ── Initialize coordinator ──
    ai_kv::core::Coordinator coordinator(config);

    auto transport = std::make_shared<GrpcRaftTransport>(config.peers);
    coordinator.SetTransport(transport);

    coordinator.Start();

    // ── Build and start gRPC server ──
    // grpc::ServerBuilder builder;
    // builder.AddListeningPort(config.address, grpc::InsecureServerCredentials());
    // builder.RegisterService(&kv_service);
    // builder.RegisterService(&raft_service);
    // builder.RegisterService(&migration_service);
    // auto server = builder.BuildAndStart();

    std::cout << "[Main] Server listening on " << config.address << "\n";
    std::cout << "[Main] Press Ctrl+C to shutdown.\n\n";

    // ── Wait for shutdown signal ──
    while (!g_shutdown.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::cout << "\n[Main] Shutting down...\n";
    coordinator.Shutdown();
    // server->Shutdown();

    return 0;
}
