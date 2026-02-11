#pragma once

/**
 * Platform compatibility layer for threading primitives.
 *
 * MinGW 6.3 with win32 thread model doesn't support <mutex>, <thread>,
 * <condition_variable>.  This header provides dcs::compat:: types that
 * wrap Win32 APIs.  On modern compilers, they alias the std:: types.
 *
 * All project code uses: Mutex, LockGuard, UniqueLock, CondVar, Thread, Atomic.
 */

#if defined(__MINGW32__) && !defined(_GLIBCXX_HAS_GTHREADS)
    #define DCS_USE_WIN32_THREADS 1
#else
    #define DCS_USE_WIN32_THREADS 0
#endif

#include <chrono>
#include <cstdint>
#include <functional>
#include <utility>

#if DCS_USE_WIN32_THREADS

// ════════════════════════════════════════════════════════════════════
//  Win32 implementation
// ════════════════════════════════════════════════════════════════════

#ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <process.h>

namespace dcs {
namespace compat {

class Mutex {
public:
    Mutex() { InitializeCriticalSection(&cs_); }
    ~Mutex() { DeleteCriticalSection(&cs_); }
    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;
    void lock() { EnterCriticalSection(&cs_); }
    void unlock() { LeaveCriticalSection(&cs_); }
    CRITICAL_SECTION& native() { return cs_; }
private:
    CRITICAL_SECTION cs_;
};

template<class M>
class LockGuard {
public:
    explicit LockGuard(M& m) : m_(m) { m_.lock(); }
    ~LockGuard() { m_.unlock(); }
    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;
private:
    M& m_;
};

template<class M>
class UniqueLock {
public:
    explicit UniqueLock(M& m) : m_(&m), owns_(true) { m_->lock(); }
    ~UniqueLock() { if (owns_) m_->unlock(); }
    UniqueLock(const UniqueLock&) = delete;
    UniqueLock& operator=(const UniqueLock&) = delete;
    void lock() { m_->lock(); owns_ = true; }
    void unlock() { m_->unlock(); owns_ = false; }
    M* mutex() const { return m_; }
private:
    M* m_;
    bool owns_;
};

class CondVar {
public:
    CondVar() { event_ = CreateEvent(NULL, FALSE, FALSE, NULL); }
    ~CondVar() { CloseHandle(event_); }
    void notify_one() { SetEvent(event_); }
    void notify_all() { SetEvent(event_); }

    void wait(UniqueLock<Mutex>& lock) {
        lock.unlock();
        WaitForSingleObject(event_, INFINITE);
        lock.lock();
    }

    template<class Pred>
    void wait(UniqueLock<Mutex>& lock, Pred pred) {
        while (!pred()) {
            lock.unlock();
            WaitForSingleObject(event_, 50);
            lock.lock();
        }
    }

    template<class Rep, class Period, class Pred>
    bool wait_for(UniqueLock<Mutex>& lock,
                  const std::chrono::duration<Rep, Period>& dur, Pred pred) {
        auto deadline = std::chrono::steady_clock::now() + dur;
        while (!pred()) {
            auto rem = deadline - std::chrono::steady_clock::now();
            if (rem <= std::chrono::milliseconds(0)) return pred();
            long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(rem).count();
            lock.unlock();
            WaitForSingleObject(event_, static_cast<DWORD>(ms > 0 ? ms : 1));
            lock.lock();
        }
        return true;
    }
private:
    HANDLE event_;
};

class Thread {
public:
    Thread() : handle_(NULL) {}

    template<class Fn, class... Args>
    explicit Thread(Fn&& fn, Args&&... args) {
        auto* task = new std::function<void()>(
            std::bind(std::forward<Fn>(fn), std::forward<Args>(args)...)
        );
        unsigned id;
        handle_ = (HANDLE)_beginthreadex(nullptr, 0, &run, task, 0, &id);
    }

    ~Thread() {}

    Thread(Thread&& o) noexcept : handle_(o.handle_) { o.handle_ = NULL; }
    Thread& operator=(Thread&& o) noexcept {
        if (this != &o) {
            if (joinable()) { WaitForSingleObject(handle_, INFINITE); CloseHandle(handle_); }
            handle_ = o.handle_; o.handle_ = NULL;
        }
        return *this;
    }
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;

    bool joinable() const { return handle_ != NULL; }
    void join() {
        if (joinable()) {
            WaitForSingleObject(handle_, INFINITE);
            CloseHandle(handle_);
            handle_ = NULL;
        }
    }
    void detach() {
        if (joinable()) { CloseHandle(handle_); handle_ = NULL; }
    }
private:
    static unsigned __stdcall run(void* arg) {
        auto* t = static_cast<std::function<void()>*>(arg);
        (*t)();
        delete t;
        return 0;
    }
    HANDLE handle_;
};

// ── Atomic (all via CRITICAL_SECTION for 32-bit safety) ────────────
template<typename T>
class Atomic {
public:
    Atomic() : val_() { InitializeCriticalSection(&cs_); }
    Atomic(T v) : val_(v) { InitializeCriticalSection(&cs_); }
    ~Atomic() { DeleteCriticalSection(&cs_); }

    // Non-copyable
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    T load() const {
        EnterCriticalSection(const_cast<CRITICAL_SECTION*>(&cs_));
        T v = val_;
        LeaveCriticalSection(const_cast<CRITICAL_SECTION*>(&cs_));
        return v;
    }

    void store(T v) {
        EnterCriticalSection(&cs_);
        val_ = v;
        LeaveCriticalSection(&cs_);
    }

    T exchange(T v) {
        EnterCriticalSection(&cs_);
        T old = val_;
        val_ = v;
        LeaveCriticalSection(&cs_);
        return old;
    }

    T fetch_add(T v) {
        EnterCriticalSection(&cs_);
        T old = val_;
        val_ += v;
        LeaveCriticalSection(&cs_);
        return old;
    }

    T operator++(int) { return fetch_add(1); }
    T operator++() { return fetch_add(1) + 1; }
    operator T() const { return load(); }
    Atomic& operator=(T v) { store(v); return *this; }
private:
    T val_;
    CRITICAL_SECTION cs_;
};

}  // namespace compat
}  // namespace dcs

#else

// ════════════════════════════════════════════════════════════════════
//  Standard library — alias into dcs::compat
// ════════════════════════════════════════════════════════════════════

#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>

namespace dcs {
namespace compat {

using Mutex = std::mutex;

template<class T>
using LockGuard = std::lock_guard<T>;

template<class T>
using UniqueLock = std::unique_lock<T>;

using CondVar = std::condition_variable;
using Thread = std::thread;

template<class T>
using Atomic = std::atomic<T>;

}  // namespace compat
}  // namespace dcs

#endif
