#pragma once
// ────────────────────────────────────────────────────────────────
// Tensor: Minimal 2D matrix for neural network computations.
// Supports MatMul, element-wise ops, Tanh, Xavier init, SGD/Adam.
// Pure C++ — no external dependencies.
// ────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

namespace dcs {
namespace ml {

class Tensor {
public:
    Tensor() : rows_(0), cols_(0) {}
    Tensor(size_t rows, size_t cols, float fill = 0.0f)
        : rows_(rows), cols_(cols), data_(rows * cols, fill) {}

    // ─── Factory ───────────────────────────────────────────────

    static Tensor Zeros(size_t r, size_t c) { return Tensor(r, c, 0.0f); }
    static Tensor Ones(size_t r, size_t c)  { return Tensor(r, c, 1.0f); }

    static Tensor Xavier(size_t r, size_t c, unsigned seed = 42) {
        Tensor t(r, c);
        std::mt19937 gen(seed);
        float limit = std::sqrt(6.0f / static_cast<float>(r + c));
        std::uniform_real_distribution<float> dist(-limit, limit);
        for (auto& v : t.data_) v = dist(gen);
        return t;
    }

    static Tensor Random(size_t r, size_t c, float lo, float hi, unsigned seed = 42) {
        Tensor t(r, c);
        std::mt19937 gen(seed);
        std::uniform_real_distribution<float> dist(lo, hi);
        for (auto& v : t.data_) v = dist(gen);
        return t;
    }

    // ─── Access ────────────────────────────────────────────────

    float& operator()(size_t r, size_t c)       { return data_[r * cols_ + c]; }
    float  operator()(size_t r, size_t c) const  { return data_[r * cols_ + c]; }

    size_t Rows() const { return rows_; }
    size_t Cols() const { return cols_; }
    size_t Size() const { return data_.size(); }
    float* Data()       { return data_.data(); }
    const float* Data() const { return data_.data(); }

    // ─── Matrix Operations ─────────────────────────────────────

    Tensor MatMul(const Tensor& b) const {
        assert(cols_ == b.rows_);
        Tensor out(rows_, b.cols_);
        for (size_t i = 0; i < rows_; i++) {
            for (size_t k = 0; k < cols_; k++) {
                float a_ik = data_[i * cols_ + k];
                for (size_t j = 0; j < b.cols_; j++) {
                    out.data_[i * b.cols_ + j] += a_ik * b.data_[k * b.cols_ + j];
                }
            }
        }
        return out;
    }

    Tensor Transpose() const {
        Tensor out(cols_, rows_);
        for (size_t i = 0; i < rows_; i++)
            for (size_t j = 0; j < cols_; j++)
                out.data_[j * rows_ + i] = data_[i * cols_ + j];
        return out;
    }

    // ─── Element-wise Operations ───────────────────────────────

    Tensor operator+(const Tensor& o) const {
        assert(SameShape(o));
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = data_[i] + o.data_[i];
        return out;
    }

    Tensor operator-(const Tensor& o) const {
        assert(SameShape(o));
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = data_[i] - o.data_[i];
        return out;
    }

    Tensor operator*(const Tensor& o) const {
        assert(SameShape(o));
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = data_[i] * o.data_[i];
        return out;
    }

    Tensor operator*(float s) const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = data_[i] * s;
        return out;
    }

    // Broadcast add: add a row vector to every row
    Tensor AddBias(const Tensor& bias) const {
        assert(bias.rows_ == 1 && bias.cols_ == cols_);
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < rows_; i++)
            for (size_t j = 0; j < cols_; j++)
                out.data_[i * cols_ + j] = data_[i * cols_ + j] + bias.data_[j];
        return out;
    }

    // ─── Activation Functions ──────────────────────────────────

    Tensor Tanh() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = std::tanh(data_[i]);
        return out;
    }

    Tensor TanhGrad() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++) {
            float t = std::tanh(data_[i]);
            out.data_[i] = 1.0f - t * t;
        }
        return out;
    }

    Tensor Sigmoid() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = 1.0f / (1.0f + std::exp(-data_[i]));
        return out;
    }

    Tensor ReLU() const {
        Tensor out(rows_, cols_);
        for (size_t i = 0; i < data_.size(); i++)
            out.data_[i] = std::max(0.0f, data_[i]);
        return out;
    }

    // ─── Reduction ─────────────────────────────────────────────

    float Sum() const {
        float s = 0;
        for (float v : data_) s += v;
        return s;
    }

    float Mean() const {
        return data_.empty() ? 0.0f : Sum() / static_cast<float>(data_.size());
    }

    // Sum along rows → 1×cols
    Tensor SumRows() const {
        Tensor out(1, cols_);
        for (size_t i = 0; i < rows_; i++)
            for (size_t j = 0; j < cols_; j++)
                out.data_[j] += data_[i * cols_ + j];
        return out;
    }

    // ─── Optimizers ────────────────────────────────────────────

    void SGDUpdate(const Tensor& grad, float lr) {
        for (size_t i = 0; i < data_.size(); i++)
            data_[i] -= lr * grad.data_[i];
    }

    void AdamUpdate(const Tensor& grad, struct AdamState& state,
                    float lr = 1e-3f, float beta1 = 0.9f, float beta2 = 0.999f,
                    float eps = 1e-8f);

private:
    bool SameShape(const Tensor& o) const {
        return rows_ == o.rows_ && cols_ == o.cols_;
    }

    size_t rows_, cols_;
    std::vector<float> data_;
};

// AdamState must be defined after Tensor is complete
struct AdamState {
    Tensor m, v;
    int t = 0;
    void Init(size_t r, size_t c) {
        m = Tensor::Zeros(r, c);
        v = Tensor::Zeros(r, c);
        t = 0;
    }
};

inline void Tensor::AdamUpdate(const Tensor& grad, AdamState& state,
                float lr, float beta1, float beta2, float eps) {
    state.t++;
    for (size_t i = 0; i < data_.size(); i++) {
        state.m.data_[i] = beta1 * state.m.data_[i] + (1 - beta1) * grad.data_[i];
        state.v.data_[i] = beta2 * state.v.data_[i] + (1 - beta2) * grad.data_[i] * grad.data_[i];
        float m_hat = state.m.data_[i] / (1.0f - std::pow(beta1, state.t));
        float v_hat = state.v.data_[i] / (1.0f - std::pow(beta2, state.t));
        data_[i] -= lr * m_hat / (std::sqrt(v_hat) + eps);
    }
}

}  // namespace ml
}  // namespace dcs
