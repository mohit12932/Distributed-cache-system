// ai_kv_store/include/ml/pinn_model.h
// ────────────────────────────────────────────────────────────────
// Physics-Informed Neural Network (PINN) for traffic prediction.
//
// Architecture:  [t, x] → Dense(2→64, tanh) → Dense(64→64, tanh)
//                       → Dense(64→64, tanh) → Dense(64→64, tanh)
//                       → Dense(64→1, linear) → û(t, x)
//
// PDE Constraint: Burgers' Equation
//   ∂u/∂t + u·∂u/∂x = ν·∂²u/∂x²
//
// Derivatives computed via finite-difference approximation through
// the network (numerical autograd). For production, use a tape-based
// AD library (e.g., CppAD, Enzyme, or link to PyTorch C++ frontend).
// ────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include "tensor.h"

namespace ai_kv {
namespace ml {

// ── Layer: Dense (fully connected) ──

struct DenseLayer {
    Tensor weights;     // [in_features × out_features]
    Tensor bias;        // [1 × out_features]
    bool   use_tanh;    // Activation function

    // Adam optimizer state
    Tensor::AdamState w_adam;
    Tensor::AdamState b_adam;

    DenseLayer(size_t in_features, size_t out_features, bool activation, uint32_t seed)
        : weights(in_features, out_features),
          bias(1, out_features, 0.0f),
          use_tanh(activation),
          w_adam(in_features, out_features),
          b_adam(1, out_features) {
        weights.XavierInit(in_features, out_features, seed);
    }

    // Forward pass: Y = activation(X·W + b)
    Tensor Forward(const Tensor& input) const {
        Tensor z = Tensor::MatMul(input, weights).AddBias(bias);
        return use_tanh ? z.Tanh() : z;
    }

    // Pre-activation (needed for gradient computation)
    Tensor PreActivation(const Tensor& input) const {
        return Tensor::MatMul(input, weights).AddBias(bias);
    }
};

// ── PINN Configuration ──

struct PINNConfig {
    int    hidden_layers   = 4;
    int    hidden_dim      = 64;
    float  viscosity       = 0.01f;    // ν in Burgers' equation
    float  learning_rate   = 1e-3f;
    float  lambda_pde      = 1.0f;     // PDE residual loss weight
    float  lambda_bc       = 0.1f;     // Boundary condition weight
    float  lambda_ic       = 10.0f;    // Initial condition weight
    float  fd_epsilon      = 1e-4f;    // Finite-difference step size
    int    num_shards      = 8;        // Total shard count S
};

// ── Training data point ──

struct TrafficSample {
    float t;       // Normalized time
    float x;       // Normalized shard position [0, S]
    float u_obs;   // Observed traffic density (QPS normalized)
};

// ── PINN Model ──

class PINNModel {
public:
    explicit PINNModel(const PINNConfig& config = PINNConfig{})
        : config_(config), train_step_(0) {
        BuildNetwork();
    }

    // ── Forward pass: predict traffic density at (t, x) ──

    float Predict(float t, float x) const {
        Tensor input(1, 2);
        input(0, 0) = t;
        input(0, 1) = x;
        Tensor output = Forward(input);
        return output(0, 0);
    }

    // ── Batch forward pass ──

    Tensor Forward(const Tensor& input) const {
        // input: [batch × 2] where col0=t, col1=x
        Tensor h = input;
        for (const auto& layer : layers_) {
            h = layer.Forward(h);
        }
        return h;  // [batch × 1]
    }

    // ── Compute PDE residual: f = u_t + u·u_x − ν·u_xx ──
    //    Uses finite-difference approximation for derivatives

    struct PDEResidual {
        float u;      // u(t, x)
        float u_t;    // ∂u/∂t
        float u_x;    // ∂u/∂x
        float u_xx;   // ∂²u/∂x²
        float f;      // Residual: u_t + u·u_x − ν·u_xx
    };

    PDEResidual ComputeResidual(float t, float x) const {
        float eps = config_.fd_epsilon;
        float nu  = config_.viscosity;

        // Central differences for derivatives
        float u    = Predict(t, x);

        // ∂u/∂t ≈ (u(t+ε, x) − u(t−ε, x)) / (2ε)
        float u_tp = Predict(t + eps, x);
        float u_tm = Predict(t - eps, x);
        float u_t  = (u_tp - u_tm) / (2.0f * eps);

        // ∂u/∂x ≈ (u(t, x+ε) − u(t, x−ε)) / (2ε)
        float u_xp = Predict(t, x + eps);
        float u_xm = Predict(t, x - eps);
        float u_x  = (u_xp - u_xm) / (2.0f * eps);

        // ∂²u/∂x² ≈ (u(t, x+ε) − 2u(t,x) + u(t, x−ε)) / ε²
        float u_xx = (u_xp - 2.0f * u + u_xm) / (eps * eps);

        // Burgers' equation residual
        float f = u_t + u * u_x - nu * u_xx;

        return {u, u_t, u_x, u_xx, f};
    }

    // ── Loss computation ──

    struct LossComponents {
        float data_loss;    // L_data: MSE of observed vs predicted
        float pde_loss;     // L_PDE: MSE of Burgers' residual
        float bc_loss;      // L_BC:  periodic boundary condition
        float ic_loss;      // L_IC:  initial condition anchoring
        float total_loss;   // Weighted sum
    };

    // Compute full PINN loss over training data and collocation points.
    //
    //   L_total = L_data + λ_r·L_PDE + λ_b·L_BC + λ_i·L_IC
    //
    //   L_data = (1/N_d) Σ |û(t_i, x_i) − u_obs_i|²
    //   L_PDE  = (1/N_r) Σ |u_t + u·u_x − ν·u_xx|²
    //   L_BC   = (1/N_b) Σ |û(t_k, 0) − û(t_k, S)|²
    //   L_IC   = (1/N_i) Σ |û(t_0, x_m) − u_obs(t_0, x_m)|²

    LossComponents ComputeLoss(
            const std::vector<TrafficSample>& data_points,
            const std::vector<std::pair<float, float>>& collocation_points,
            const std::vector<TrafficSample>& ic_points,
            float t_boundary_sample_count = 10) const {

        LossComponents loss{0, 0, 0, 0, 0};

        // ── Data fidelity loss ──
        if (!data_points.empty()) {
            float sum_sq = 0.0f;
            for (const auto& dp : data_points) {
                float pred = Predict(dp.t, dp.x);
                float err  = pred - dp.u_obs;
                sum_sq += err * err;
            }
            loss.data_loss = sum_sq / static_cast<float>(data_points.size());
        }

        // ── PDE residual loss (Burgers' equation) ──
        if (!collocation_points.empty()) {
            float sum_sq = 0.0f;
            for (const auto& cp : collocation_points) {
                PDEResidual res = ComputeResidual(cp.first, cp.second);
                sum_sq += res.f * res.f;
            }
            loss.pde_loss = sum_sq / static_cast<float>(collocation_points.size());
        }

        // ── Boundary condition loss (periodic: u(t, 0) = u(t, S)) ──
        {
            float S = static_cast<float>(config_.num_shards);
            float sum_sq = 0.0f;
            int n_bc = static_cast<int>(t_boundary_sample_count);
            for (int i = 0; i < n_bc; ++i) {
                float t = static_cast<float>(i) / static_cast<float>(n_bc);
                float u_left  = Predict(t, 0.0f);
                float u_right = Predict(t, S);
                float diff = u_left - u_right;
                sum_sq += diff * diff;
            }
            loss.bc_loss = sum_sq / static_cast<float>(n_bc);
        }

        // ── Initial condition loss ──
        if (!ic_points.empty()) {
            float sum_sq = 0.0f;
            for (const auto& ic : ic_points) {
                float pred = Predict(ic.t, ic.x);
                float err  = pred - ic.u_obs;
                sum_sq += err * err;
            }
            loss.ic_loss = sum_sq / static_cast<float>(ic_points.size());
        }

        // ── Weighted total ──
        loss.total_loss = loss.data_loss
                        + config_.lambda_pde * loss.pde_loss
                        + config_.lambda_bc  * loss.bc_loss
                        + config_.lambda_ic  * loss.ic_loss;

        return loss;
    }

    // ── Training step (numerical gradient descent) ──
    //    Uses parameter perturbation for gradient estimation.
    //    For production, replace with tape-based autograd or link to
    //    PyTorch/TensorFlow C++ frontend.

    LossComponents TrainStep(
            const std::vector<TrafficSample>& data_points,
            const std::vector<std::pair<float, float>>& collocation_points,
            const std::vector<TrafficSample>& ic_points) {

        float lr = config_.learning_rate;
        float eps = 1e-4f;

        auto base_loss = ComputeLoss(data_points, collocation_points, ic_points);

        // Perturb each parameter and estimate gradient
        for (auto& layer : layers_) {
            // Update weights
            for (size_t i = 0; i < layer.weights.Size(); ++i) {
                float original = layer.weights.Data()[i];

                layer.weights.Data()[i] = original + eps;
                float loss_plus = ComputeLoss(data_points, collocation_points,
                                              ic_points).total_loss;

                layer.weights.Data()[i] = original - eps;
                float loss_minus = ComputeLoss(data_points, collocation_points,
                                               ic_points).total_loss;

                layer.weights.Data()[i] = original;

                float grad = (loss_plus - loss_minus) / (2.0f * eps);
                layer.weights.Data()[i] -= lr * grad;
            }

            // Update biases
            for (size_t i = 0; i < layer.bias.Size(); ++i) {
                float original = layer.bias.Data()[i];

                layer.bias.Data()[i] = original + eps;
                float loss_plus = ComputeLoss(data_points, collocation_points,
                                              ic_points).total_loss;

                layer.bias.Data()[i] = original - eps;
                float loss_minus = ComputeLoss(data_points, collocation_points,
                                               ic_points).total_loss;

                layer.bias.Data()[i] = original;

                float grad = (loss_plus - loss_minus) / (2.0f * eps);
                layer.bias.Data()[i] -= lr * grad;
            }
        }

        ++train_step_;
        return base_loss;
    }

    // ── Batch prediction: predict heat for all shards at future time ──

    struct ShardPrediction {
        int   shard_id;
        float predicted_heat;
        float gradient;   // ∂u/∂t at current time
    };

    std::vector<ShardPrediction> PredictHeatMap(float t_now,
                                                 float t_horizon) const {
        std::vector<ShardPrediction> predictions;
        float S = static_cast<float>(config_.num_shards);

        for (int s = 0; s < config_.num_shards; ++s) {
            float x = static_cast<float>(s) + 0.5f;  // Center of shard

            float heat_future  = Predict(t_now + t_horizon, x);
            float heat_current = Predict(t_now, x);
            float gradient     = (heat_future - heat_current) / t_horizon;

            predictions.push_back({s, heat_future, gradient});
        }

        return predictions;
    }

    // ── Serialization (for double-buffered model swap) ──

    struct Snapshot {
        std::vector<std::pair<std::vector<float>, std::vector<float>>> layer_params;
    };

    Snapshot TakeSnapshot() const {
        Snapshot snap;
        for (const auto& layer : layers_) {
            std::vector<float> w(layer.weights.Data(),
                                 layer.weights.Data() + layer.weights.Size());
            std::vector<float> b(layer.bias.Data(),
                                 layer.bias.Data() + layer.bias.Size());
            snap.layer_params.emplace_back(std::move(w), std::move(b));
        }
        return snap;
    }

    void LoadSnapshot(const Snapshot& snap) {
        assert(snap.layer_params.size() == layers_.size());
        for (size_t i = 0; i < layers_.size(); ++i) {
            std::copy(snap.layer_params[i].first.begin(),
                      snap.layer_params[i].first.end(),
                      layers_[i].weights.Data());
            std::copy(snap.layer_params[i].second.begin(),
                      snap.layer_params[i].second.end(),
                      layers_[i].bias.Data());
        }
    }

    // ── Accessors ──

    const PINNConfig& Config() const { return config_; }
    int TrainStepCount() const { return train_step_; }

private:
    void BuildNetwork() {
        uint32_t seed = 42;
        int in_dim = 2;  // [t, x]

        // Input → first hidden
        layers_.emplace_back(in_dim, config_.hidden_dim, true, seed++);

        // Hidden layers
        for (int i = 1; i < config_.hidden_layers; ++i) {
            layers_.emplace_back(config_.hidden_dim, config_.hidden_dim, true, seed++);
        }

        // Last hidden → output (no activation — linear)
        layers_.emplace_back(config_.hidden_dim, 1, false, seed++);
    }

    PINNConfig               config_;
    std::vector<DenseLayer>  layers_;
    int                      train_step_;
};

// ── Double-buffered model for lock-free inference during training ──

class DoubleBufferedPINN {
public:
    explicit DoubleBufferedPINN(const PINNConfig& config = PINNConfig{})
        : model_a_(std::make_shared<PINNModel>(config)),
          model_b_(std::make_shared<PINNModel>(config)),
          active_(model_a_) {}

    // Thread-safe read: returns current active model
    std::shared_ptr<const PINNModel> ActiveModel() const {
        return std::atomic_load(&active_);
    }

    // Called by trainer thread: get the inactive model, train it, then swap
    PINNModel* InactiveModel() {
        auto current = std::atomic_load(&active_);
        return (current.get() == model_a_.get()) ? model_b_.get() : model_a_.get();
    }

    // Atomic swap after training completes
    void SwapModels() {
        auto current = std::atomic_load(&active_);
        auto next = (current.get() == model_a_.get()) ? model_b_ : model_a_;

        // Copy trained weights to the new active before swapping
        // (The trainer was working on InactiveModel(), so it's already up to date)
        std::atomic_store(&active_, next);
    }

private:
    std::shared_ptr<PINNModel>       model_a_;
    std::shared_ptr<PINNModel>       model_b_;
    std::shared_ptr<PINNModel>       active_;
};

}  // namespace ml
}  // namespace ai_kv
