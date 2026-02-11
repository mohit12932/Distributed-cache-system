#pragma once

#include "storage_backend.h"
#include "../compat/threading.h"

#include <string>
#include <unordered_map>
#include <fstream>
#include <sstream>

namespace dcs {
namespace persistence {

/**
 * FileStorage — A simple JSON-like file-backed key-value store.
 *
 * Format on disk (one entry per line):  KEY\tVALUE\n
 * On startup the entire file is loaded into an in-memory map; writes
 * rewrite the full file (simple but correct for demonstration).
 *
 * Thread-safety: internal mutex protects file I/O.
 */
class FileStorage : public StorageBackend {
public:
    explicit FileStorage(const std::string& filepath)
        : filepath_(filepath) {
        load_from_disk();
    }

    ~FileStorage() override = default;

    LoadResult load(const std::string& key) override {
        compat::LockGuard<compat::Mutex> lock(mu_);
        auto it = data_.find(key);
        if (it == data_.end()) return LoadResult::Miss();
        return LoadResult::Hit(it->second);
    }

    bool store(const std::string& key, const std::string& value) override {
        compat::LockGuard<compat::Mutex> lock(mu_);
        data_[key] = value;
        return flush_to_disk();
    }

    bool remove(const std::string& key) override {
        compat::LockGuard<compat::Mutex> lock(mu_);
        auto it = data_.find(key);
        if (it == data_.end()) return false;
        data_.erase(it);
        return flush_to_disk();
    }

    bool batch_store(const std::vector<std::pair<std::string, std::string>>& entries) override {
        compat::LockGuard<compat::Mutex> lock(mu_);
        for (auto it = entries.begin(); it != entries.end(); ++it) {
            data_[it->first] = it->second;
        }
        return flush_to_disk();
    }

    bool ping() override {
        return true;  // local file is always "available"
    }

    /** Return total keys stored on disk. */
    size_t disk_size() const {
        compat::LockGuard<compat::Mutex> lock(mu_);
        return data_.size();
    }

private:
    void load_from_disk() {
        std::ifstream in(filepath_);
        if (!in.is_open()) return;  // file may not exist yet

        std::string line;
        while (std::getline(in, line)) {
            auto tab = line.find('\t');
            if (tab == std::string::npos) continue;
            std::string key = line.substr(0, tab);
            std::string val = line.substr(tab + 1);
            data_[key] = val;
        }
    }

    bool flush_to_disk() {
        // Ensure the parent directory exists (platform-agnostic)
        ensure_parent_dir(filepath_);

        std::ofstream out(filepath_, std::ios::trunc);
        if (!out.is_open()) return false;

        for (auto it = data_.begin(); it != data_.end(); ++it) {
            out << it->first << '\t' << it->second << '\n';
        }
        out.flush();
        return out.good();
    }

    static void ensure_parent_dir(const std::string& filepath) {
        // Find last separator
        size_t pos = filepath.find_last_of("/\\");
        if (pos == std::string::npos) return;
        std::string dir = filepath.substr(0, pos);
        if (dir.empty()) return;
#ifdef _WIN32
        std::string cmd = "mkdir \"" + dir + "\" 2>nul";
#else
        std::string cmd = "mkdir -p \"" + dir + "\"";
#endif
        system(cmd.c_str());
    }

    std::string filepath_;
    std::unordered_map<std::string, std::string> data_;
    mutable compat::Mutex mu_;
};

}  // namespace persistence
}  // namespace dcs
