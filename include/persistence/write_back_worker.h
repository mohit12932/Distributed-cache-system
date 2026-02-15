#pragma once

#include "storage_backend.h"
#include "../compat/threading.h"

#include <functional>
#include <chrono>
#include <vector>
#include <iostream>

namespace dcs {
namespace persistence {

/**
 * WriteBackWorker — Background thread for Write-Behind persistence.
 *
 * Periodically wakes up (every `interval` seconds) and calls a user-supplied
 * collector function to obtain all dirty entries, then batch-writes them to
 * the storage backend.  Also supports manual flush (for graceful shutdown).
 *
 * Lifecycle:
 *   1. Construct with a StorageBackend*, interval, and dirty-entry collector.
 *   2. Call start().
 *   3. ...server runs...
 *   4. Call stop() — flushes remaining dirty data and joins the thread.
 */
class WriteBackWorker {
public:
    using DirtyCollector = std::function<std::vector<std::pair<std::string, std::string>>()>;
    using DirtyClearer  = std::function<void(const std::string& key)>;

    WriteBackWorker(StorageBackend* backend,
                    std::chrono::seconds interval,
                    DirtyCollector collector,
                    DirtyClearer clearer)
        : backend_(backend)
        , interval_(interval)
        , collector_(std::move(collector))
        , clearer_(std::move(clearer))
        , running_(false)
        , flush_count_(0) {}

    ~WriteBackWorker() {
        stop();
    }

    void start() {
        if (running_.exchange(true)) return;  // already running
        thread_ = compat::Thread([this] { run_loop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();

        // Final flush on shutdown
        flush();
    }

    /** Force an immediate flush (e.g. before shutdown). */
    void flush() {
        auto dirty = collector_();
        if (dirty.empty()) return;

        // Flush in smaller batches to limit peak memory usage (32-bit safe)
        const size_t BATCH_LIMIT = 5000;
        size_t offset = 0;
        while (offset < dirty.size()) {
            size_t end = std::min(offset + BATCH_LIMIT, dirty.size());
            std::vector<std::pair<std::string, std::string>> batch(dirty.begin() + offset, dirty.begin() + end);
            if (backend_->batch_store(batch)) {
                for (size_t i = 0; i < batch.size(); ++i) {
                    clearer_(batch[i].first);
                }
            } else {
                std::cerr << "[WriteBack] ERROR: batch_store failed!\n";
                return;
            }
            offset = end;
        }
        flush_count_.fetch_add(1);
        std::cout << "[WriteBack] Flushed " << dirty.size()
                  << " dirty entries to disk.\n";
    }

    /** Trigger an out-of-cycle flush (e.g. dirty set size exceeded). */
    void notify_flush() {
        cv_.notify_one();
    }

    uint64_t flush_count() const {
        return flush_count_.load();
    }

private:
    void run_loop() {
        while (running_.load()) {
            compat::UniqueLock<compat::Mutex> lock(mu_);
            cv_.wait_for(lock, interval_, [this] { return !running_.load(); });

            if (!running_.load()) break;

            // Perform the flush outside the lock
            lock.unlock();
            flush();
        }
    }

    StorageBackend* backend_;
    std::chrono::seconds interval_;
    DirtyCollector collector_;
    DirtyClearer clearer_;

    compat::Atomic<bool> running_;
    compat::Thread thread_;
    compat::Mutex mu_;
    compat::CondVar cv_;
    compat::Atomic<uint64_t> flush_count_;
};

}  // namespace persistence
}  // namespace dcs
