#pragma once
// ────────────────────────────────────────────────────────────────
// Predictive Sharder: Uses PINN model to predict cache shard load
// and recommend migrations. Collects telemetry from shards,
// trains the PINN model periodically, and evaluates predictions.
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "../compat/threading.h"
#include "pinn_model.h"
#include "tensor.h"

namespace dcs {
namespace ml {

struct ShardTelemetry {
    int     shard_id;
    float   load;        // normalized 0..1
    float   hit_rate;
    float   latency_ms;
    float   timestamp;   // normalized time
};

struct MigrationRecommendation {
    int   from_shard;
    int   to_shard;
    float predicted_load_from;
    float predicted_load_to;
    float confidence;
};

class PredictiveSharder {
public:
    static constexpr size_t kRingBufferSize = 1024;
    static constexpr int    kTrainBatchSize = 64;
    static constexpr int    kTrainInterval  = 5;   // seconds between training

    explicit PredictiveSharder(int num_shards, const PINNConfig& config = PINNConfig())
        : num_shards_(num_shards), pinn_(config), running_(false),
          start_time_(std::chrono::steady_clock::now()),
          telemetry_head_(0), telemetry_count_(0) {
        telemetry_.resize(kRingBufferSize);
    }

    ~PredictiveSharder() { Stop(); }

    void Start() {
        running_ = true;
        trainer_thread_ = compat::Thread(&PredictiveSharder::TrainerLoop, this);
    }

    void Stop() {
        running_ = false;
        if (trainer_thread_.joinable()) trainer_thread_.join();
    }

    // ─── Telemetry Collection ──────────────────────────────────

    void RecordTelemetry(int shard_id, float load, float hit_rate, float latency_ms) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        float t = CurrentTime();
        size_t idx = telemetry_head_ % kRingBufferSize;
        telemetry_[idx] = {shard_id, load, hit_rate, latency_ms, t};
        telemetry_head_++;
        if (telemetry_count_ < kRingBufferSize) telemetry_count_++;
    }

    // ─── Predictions ───────────────────────────────────────────

    std::vector<float> PredictLoads(float future_time_offset = 0.0f) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        float t = CurrentTime() + future_time_offset;
        return pinn_.PredictAllShards(num_shards_, t);
    }

    float PredictShardLoad(int shard_id, float future_time_offset = 0.0f) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        float t = CurrentTime() + future_time_offset;
        float x = static_cast<float>(shard_id) / static_cast<float>(num_shards_);
        return pinn_.Predict(x, t);
    }

    // ─── Migration Recommendations ─────────────────────────────

    std::vector<MigrationRecommendation> GetRecommendations(float threshold = 0.7f) {
        std::vector<float> loads = PredictLoads(1.0f);  // predict 1 step ahead
        std::vector<MigrationRecommendation> recs;

        // Find overloaded and underloaded shards
        float mean_load = 0;
        for (float l : loads) mean_load += l;
        mean_load /= static_cast<float>(loads.size());

        for (int i = 0; i < num_shards_; i++) {
            if (loads[i] > threshold) {
                // Find the least loaded shard
                int min_shard = 0;
                for (int j = 1; j < num_shards_; j++) {
                    if (loads[j] < loads[min_shard]) min_shard = j;
                }
                if (min_shard != i && loads[min_shard] < mean_load) {
                    float confidence = std::min(1.0f,
                        (loads[i] - loads[min_shard]) / threshold);
                    recs.push_back({i, min_shard, loads[i], loads[min_shard], confidence});
                }
            }
        }
        return recs;
    }

    // ─── Stats ─────────────────────────────────────────────────

    struct SharderStats {
        int   training_steps;
        float total_loss;
        float data_loss;
        float pde_loss;
        int   num_parameters;
        size_t telemetry_count;
    };

    SharderStats GetStats() const {
        auto pinn_stats = pinn_.GetStats();
        return {
            pinn_stats.step_count,
            pinn_stats.total_loss,
            pinn_stats.data_loss,
            pinn_stats.pde_loss,
            pinn_stats.num_parameters,
            telemetry_count_
        };
    }

private:
    void TrainerLoop() {
        while (running_) {
            compat::this_thread::sleep_for(std::chrono::seconds(5));

            compat::LockGuard<compat::Mutex> lock(mu_);
            size_t count = std::min(telemetry_count_, static_cast<size_t>(kTrainBatchSize));
            if (count < 8) continue;  // need minimum data

            // Build training batch from telemetry ring buffer
            Tensor data_x(count, 2);
            Tensor data_y(count, 1);

            size_t start = (telemetry_head_ > count) ? telemetry_head_ - count : 0;
            for (size_t i = 0; i < count; i++) {
                size_t idx = (start + i) % kRingBufferSize;
                const auto& t = telemetry_[idx];
                data_x(i, 0) = static_cast<float>(t.shard_id) /
                               static_cast<float>(num_shards_);
                data_x(i, 1) = t.timestamp;
                data_y(i, 0) = t.load;
            }

            pinn_.TrainStep(data_x, data_y);
        }
    }

    float CurrentTime() const {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - start_time_).count();
        // Normalize to 0..1 range over 1 hour
        return static_cast<float>(elapsed) / 3600.0f;
    }

    int          num_shards_;
    PINNModel    pinn_;
    compat::Atomic<bool> running_;
    std::chrono::steady_clock::time_point start_time_;

    std::vector<ShardTelemetry> telemetry_;
    size_t       telemetry_head_;
    size_t       telemetry_count_;

    compat::Mutex mu_;
    compat::Thread   trainer_thread_;
};

}  // namespace ml
}  // namespace dcs
