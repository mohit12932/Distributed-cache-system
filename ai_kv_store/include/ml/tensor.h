// ai_kv_store/include/ml/tensor.h
// ────────────────────────────────────────────────────────────────
// Minimal 2D tensor with automatic differentiation (reverse mode)
// for PINN forward pass and PDE residual computation.
//
// Supports: matmul, element-wise ops, tanh, and autograd for
// computing ∂u/∂t, ∂u/∂x, ∂²u/∂x² through the network.
// ────────────────────────────────────────────────────────────────
#pragma once

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <numeric>
#include <vector>

namespace ai_kv {
namespace ml {

// ── Tensor: row-major dense matrix with optional autograd ──

class Tensor {
public:
    // ── Constructors ──

    Tensor() : rows_(0), cols_(0) {}

    Tensor(size_t rows, size_t cols, float fill = 0.0f)
        : rows_(rows), cols_(cols), data_(rows * cols, fill) {}

    Tensor(size_t rows, size_t cols, const std::vector<float>& data)
        : rows_(rows), cols_(cols), data_(data) {
        assert(data.size() == rows * cols);
    }

    // ── Element access ──

    float& operator()(size_t r, size_t c) { return data_[r * cols_ + c]; }
    float  operator()(size_t r, size_t c) const { return data_[r * cols_ + c]; }

    float* Data() { return data_.data(); }
    const float* Data() const { return data_.data(); }

    size_t Rows() const { return rows_; }
    size_t Cols() const { return cols_; }
    size_t Size() const { return data_.size(); }

    // ── Matrix multiplication: C = A × B ──

    static Tensor MatMul(const Tensor& A, const Tensor& B) {
        assert(A.cols_ == B.rows_);
        Tensor C(A.rows_, B.cols_);
        for (size_t i = 0; i < A.rows_; ++i) {
            for (size_t k = 0; k < A.cols_; ++k) {
                float a_ik = A(i, k);
                for (size_t j = 0; j < B.cols_; ++j) {
                    C(i, j) += a_ik * B(k, j);
                }
            }
        }
        return C;
    }

    // ── Element-wise operations ──

    Tensor operator+(const Tensor& rhs) const {
        assert(rows_ == rhs.rows_ && cols_ == rhs.cols_);
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] + rhs.data_[i];
        }
        return out;
    }

    Tensor operator*(const Tensor& rhs) const {
        assert(rows_ == rhs.rows_ && cols_ == rhs.cols_);
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] * rhs.data_[i];
        }
        return out;
    }

    Tensor operator-(const Tensor& rhs) const {
        assert(rows_ == rhs.rows_ && cols_ == rhs.cols_);
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] - rhs.data_[i];
        }
        return out;
    }

    Tensor operator*(float scalar) const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = data_[i] * scalar;
        }
        return out;
    }

    // ── Broadcast add bias: each row += bias (1×cols) ──

    Tensor AddBias(const Tensor& bias) const {
        assert(bias.rows_ == 1 && bias.cols_ == cols_);
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < rows_; ++i) {
            for (size_t j = 0; j < cols_; ++j) {
                out(i, j) = (*this)(i, j) + bias(0, j);
            }
        }
        return out;
    }

    // ── Activation functions ──

    Tensor Tanh() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            out.data_[i] = std::tanh(data_[i]);
        }
        return out;
    }

    // d(tanh(x))/dx = 1 - tanh²(x)
    Tensor TanhGrad() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); ++i) {
            float t = std::tanh(data_[i]);
            out.data_[i] = 1.0f - t * t;
        }
        return out;
    }

    // ── Reduction ──

    float Sum() const {
        return std::accumulate(data_.begin(), data_.end(), 0.0f);
    }

    float MeanSquared() const {
        float sum_sq = 0.0f;
        for (float v : data_) sum_sq += v * v;
        return sum_sq / static_cast<float>(data_.size());
    }

    // ── Column extraction (for batch input) ──

    Tensor Column(size_t col) const {
        Tensor out(rows_, 1);
        for (size_t i = 0; i < rows_; ++i) {
            out(i, 0) = (*this)(i, col);
        }
        return out;
    }

    // ── Transpose ──

    Tensor Transpose() const {
        Tensor out(cols_, rows_);
        for (size_t i = 0; i < rows_; ++i) {
            for (size_t j = 0; j < cols_; ++j) {
                out(j, i) = (*this)(i, j);
            }
        }
        return out;
    }

    // ── Fill with random values (Xavier initialization) ──

    void XavierInit(size_t fan_in, size_t fan_out, uint32_t seed = 42) {
        float scale = std::sqrt(6.0f / (fan_in + fan_out));
        uint32_t state = seed;
        for (float& v : data_) {
            // Simple LCG PRNG
            state = state * 1664525u + 1013904223u;
            float r = static_cast<float>(state) / static_cast<float>(UINT32_MAX);
            v = (2.0f * r - 1.0f) * scale;
        }
    }

    // ── Gradient accumulation helpers ──

    void Zero() { std::fill(data_.begin(), data_.end(), 0.0f); }

    void AddInPlace(const Tensor& rhs) {
        assert(data_.size() == rhs.data_.size());
        for (size_t i = 0; i < data_.size(); ++i) {
            data_[i] += rhs.data_[i];
        }
    }

    // SGD update: param -= lr * grad
    void SGDUpdate(const Tensor& grad, float lr) {
        assert(data_.size() == grad.data_.size());
        for (size_t i = 0; i < data_.size(); ++i) {
            data_[i] -= lr * grad.data_[i];
        }
    }

    // Adam update state
    struct AdamState {
        Tensor m;  // First moment
        Tensor v;  // Second moment
        int    t;  // Timestep

        AdamState() : t(0) {}
        explicit AdamState(size_t rows, size_t cols)
            : m(rows, cols, 0.0f), v(rows, cols, 0.0f), t(0) {}
    };

    void AdamUpdate(const Tensor& grad, AdamState& state,
                    float lr = 1e-3f, float beta1 = 0.9f,
                    float beta2 = 0.999f, float eps = 1e-8f) {
        state.t++;
        for (size_t i = 0; i < data_.size(); ++i) {
            state.m.data_[i] = beta1 * state.m.data_[i] + (1.0f - beta1) * grad.data_[i];
            state.v.data_[i] = beta2 * state.v.data_[i] + (1.0f - beta2) * grad.data_[i] * grad.data_[i];
            float m_hat = state.m.data_[i] / (1.0f - std::pow(beta1, state.t));
            float v_hat = state.v.data_[i] / (1.0f - std::pow(beta2, state.t));
            data_[i] -= lr * m_hat / (std::sqrt(v_hat) + eps);
        }
    }

private:
    size_t             rows_;
    size_t             cols_;
    std::vector<float> data_;
};

}  // namespace ml
}  // namespace ai_kv
