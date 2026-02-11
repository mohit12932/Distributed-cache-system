#pragma once

#include <string>
#include <vector>
#include <utility>

namespace dcs {
namespace persistence {

/**
 * Result of a storage load: (found, value).
 * Using pair instead of std::optional for broader compiler compat.
 */
struct LoadResult {
    bool found;
    std::string value;
    static LoadResult Hit(const std::string& v) { return {true, v}; }
    static LoadResult Miss() { return {false, ""}; }
};

/**
 * Abstract interface for a durable storage backend.
 * Implementations: FileStorage (JSON on disk), or a future PostgreSQL adapter.
 */
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    /** Read a single key. Returns LoadResult::Miss() on miss. */
    virtual LoadResult load(const std::string& key) = 0;

    /** Write a single key-value pair (upsert). */
    virtual bool store(const std::string& key, const std::string& value) = 0;

    /** Delete a key. Returns true if the key existed. */
    virtual bool remove(const std::string& key) = 0;

    /** Batch write — default implementation calls store() in a loop. */
    virtual bool batch_store(const std::vector<std::pair<std::string, std::string>>& entries) {
        for (size_t i = 0; i < entries.size(); ++i) {
            if (!store(entries[i].first, entries[i].second)) return false;
        }
        return true;
    }

    /** Check if the backend is healthy / accessible. */
    virtual bool ping() = 0;
};

}  // namespace persistence
}  // namespace dcs
