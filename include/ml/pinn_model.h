#pragma once
// ────────────────────────────────────────────────────────────────
// PINN Model: Physics-Informed Neural Network for cache load
// prediction using Burgers' equation as the physical prior.
//
// Architecture: 2 inputs (x, t) → 4 hidden layers (64 neurons)
//               → 1 output (predicted load u)
//
// Loss = MSE_data + λ * PDE_residual (Burgers' equation)
// Burgers': ∂u/∂t + u·∂u/∂x = ν·∂²u/∂x²
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

#include "tensor.h"

namespace dcs {
namespace ml {

struct PINNConfig {
    int    hidden_size    = 64;
    int    num_layers     = 4;
    float  learning_rate  = 1e-3f;
    float  lambda_pde     = 0.1f;   // PDE loss weight
    float  nu             = 0.01f;  // viscosity (Burgers')
    float  dx             = 1e-3f;  // finite difference step
};

class PINNModel {
public:
    explicit PINNModel(const PINNConfig& config = PINNConfig())
        : config_(config), total_loss_(0.0f), data_loss_(0.0f),
          pde_loss_(0.0f), step_count_(0) {
        InitWeights();
    }

    // ─── Forward Pass ──────────────────────────────────────────

    // Input: Nx2 tensor (x, t pairs)
    // Output: Nx1 tensor (predicted load)
    Tensor Forward(const Tensor& input) {
        // Layer 0: input → hidden
        activations_.clear();
        activations_.push_back(input);

        Tensor h = input.MatMul(weights_[0]).AddBias(biases_[0]).Tanh();
        activations_.push_back(h);

        // Hidden layers
        for (int i = 1; i < config_.num_layers; i++) {
            h = h.MatMul(weights_[i]).AddBias(biases_[i]).Tanh();
            activations_.push_back(h);
        }

        // Output layer
        Tensor out = h.MatMul(weights_.back()).AddBias(biases_.back());
        activations_.push_back(out);
        return out;
    }

    // ─── Training Step ─────────────────────────────────────────

    // data_x: Nx2 (shard_id, time), data_y: Nx1 (observed load)
    float TrainStep(const Tensor& data_x, const Tensor& data_y) {
        size_t N = data_x.Rows();

        // Forward pass
        Tensor pred = Forward(data_x);

        // Data loss: MSE
        Tensor diff = pred - data_y;
        data_loss_ = (diff * diff).Mean();

        // PDE residual loss (Burgers' equation via finite differences)
        pde_loss_ = ComputePDEResidual(data_x);

        total_loss_ = data_loss_ + config_.lambda_pde * pde_loss_;
        step_count_++;

        // Backward pass (simplified gradient computation)
        Tensor grad_output = diff * (2.0f / static_cast<float>(N));

        // Backprop through layers
        Tensor grad = grad_output;
        int num_weight_layers = static_cast<int>(weights_.size());

        for (int i = num_weight_layers - 1; i >= 0; i--) {
            Tensor& act_input = activations_[i];
            Tensor& pre_act = activations_[i + 1];

            // Gradient w.r.t. weights
            Tensor grad_w = act_input.Transpose().MatMul(grad);
            Tensor grad_b = grad.SumRows();

            // Update weights with Adam
            weights_[i].AdamUpdate(grad_w, adam_w_[i], config_.learning_rate);
            biases_[i].AdamUpdate(grad_b, adam_b_[i], config_.learning_rate);

            // Propagate gradient to previous layer
            if (i > 0) {
                grad = grad.MatMul(weights_[i].Transpose());
                // Tanh gradient
                Tensor tanh_grad = activations_[i].TanhGrad();
                // activations_[i] is post-tanh, need pre-tanh for gradient
                // Approximate: 1 - tanh²(x) where tanh(x) = activations_[i]
                Tensor one_minus_sq(tanh_grad.Rows(), tanh_grad.Cols(), 1.0f);
                for (size_t r = 0; r < activations_[i].Rows(); r++) {
                    for (size_t c = 0; c < activations_[i].Cols(); c++) {
                        float t = activations_[i](r, c);
                        one_minus_sq(r, c) = 1.0f - t * t;
                    }
                }
                grad = grad * one_minus_sq;
            }
        }

        return total_loss_;
    }

    // ─── Prediction ────────────────────────────────────────────

    // Predict load for a single (shard_id, time) pair
    float Predict(float shard_id, float time) {
        Tensor input(1, 2);
        input(0, 0) = shard_id;
        input(0, 1) = time;
        Tensor out = Forward(input);
        return out(0, 0);
    }

    // Predict loads for all shards at a given time
    std::vector<float> PredictAllShards(int num_shards, float time) {
        std::vector<float> predictions(num_shards);
        Tensor input(num_shards, 2);
        for (int i = 0; i < num_shards; i++) {
            input(i, 0) = static_cast<float>(i) / static_cast<float>(num_shards);
            input(i, 1) = time;
        }
        Tensor out = Forward(input);
        for (int i = 0; i < num_shards; i++) {
            predictions[i] = std::max(0.0f, out(i, 0));
        }
        return predictions;
    }

    // ─── Metrics ───────────────────────────────────────────────

    float TotalLoss() const { return total_loss_; }
    float DataLoss()  const { return data_loss_; }
    float PDELoss()   const { return pde_loss_; }
    int   StepCount() const { return step_count_; }

    struct ModelStats {
        float total_loss;
        float data_loss;
        float pde_loss;
        int   step_count;
        int   num_parameters;
    };

    ModelStats GetStats() const {
        int num_params = 0;
        for (const auto& w : weights_) num_params += static_cast<int>(w.Size());
        for (const auto& b : biases_) num_params += static_cast<int>(b.Size());
        return {total_loss_, data_loss_, pde_loss_, step_count_, num_params};
    }

private:
    void InitWeights() {
        unsigned seed = 42;
        // Input layer: 2 → hidden_size
        weights_.push_back(Tensor::Xavier(2, config_.hidden_size, seed++));
        biases_.push_back(Tensor::Zeros(1, config_.hidden_size));

        // Hidden layers
        for (int i = 1; i < config_.num_layers; i++) {
            weights_.push_back(Tensor::Xavier(config_.hidden_size, config_.hidden_size, seed++));
            biases_.push_back(Tensor::Zeros(1, config_.hidden_size));
        }

        // Output layer: hidden_size → 1
        weights_.push_back(Tensor::Xavier(config_.hidden_size, 1, seed++));
        biases_.push_back(Tensor::Zeros(1, 1));

        // Initialize Adam states
        for (auto& w : weights_) {
            AdamState s;
            s.Init(w.Rows(), w.Cols());
            adam_w_.push_back(s);
        }
        for (auto& b : biases_) {
            AdamState s;
            s.Init(b.Rows(), b.Cols());
            adam_b_.push_back(s);
        }
    }

    float ComputePDEResidual(const Tensor& input) {
        // Burgers' equation: ∂u/∂t + u·∂u/∂x = ν·∂²u/∂x²
        // Use finite differences to approximate derivatives
        float dx = config_.dx;
        float residual = 0.0f;
        size_t N = input.Rows();
        if (N == 0) return 0.0f;

        for (size_t i = 0; i < N; i++) {
            float x = input(i, 0);
            float t = input(i, 1);

            // u at (x, t)
            float u = Predict_Internal(x, t);

            // ∂u/∂t ≈ (u(x, t+dt) - u(x, t-dt)) / 2dt
            float u_tp = Predict_Internal(x, t + dx);
            float u_tm = Predict_Internal(x, t - dx);
            float du_dt = (u_tp - u_tm) / (2.0f * dx);

            // ∂u/∂x ≈ (u(x+dx, t) - u(x-dx, t)) / 2dx
            float u_xp = Predict_Internal(x + dx, t);
            float u_xm = Predict_Internal(x - dx, t);
            float du_dx = (u_xp - u_xm) / (2.0f * dx);

            // ∂²u/∂x² ≈ (u(x+dx, t) - 2u(x, t) + u(x-dx, t)) / dx²
            float d2u_dx2 = (u_xp - 2.0f * u + u_xm) / (dx * dx);

            // Burgers' residual
            float r = du_dt + u * du_dx - config_.nu * d2u_dx2;
            residual += r * r;
        }
        return residual / static_cast<float>(N);
    }

    float Predict_Internal(float x, float t) {
        Tensor input(1, 2);
        input(0, 0) = x;
        input(0, 1) = t;
        // Quick forward without storing activations
        Tensor h = input.MatMul(weights_[0]).AddBias(biases_[0]).Tanh();
        for (int i = 1; i < config_.num_layers; i++) {
            h = h.MatMul(weights_[i]).AddBias(biases_[i]).Tanh();
        }
        Tensor out = h.MatMul(weights_.back()).AddBias(biases_.back());
        return out(0, 0);
    }

    PINNConfig config_;
    std::vector<Tensor>               weights_;
    std::vector<Tensor>               biases_;
    std::vector<AdamState>    adam_w_;
    std::vector<AdamState>    adam_b_;
    std::vector<Tensor>               activations_;

    float total_loss_;
    float data_loss_;
    float pde_loss_;
    int   step_count_;
};

}  // namespace ml
}  // namespace dcs
