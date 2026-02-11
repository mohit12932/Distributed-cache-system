/**
 * Test suite for LRU Cache core engine.
 * Minimal test framework (no external deps).
 */

#include "include/cache/lru_cache.h"

#include <iostream>
#include <cassert>
#include <string>
#include <vector>

#define TEST(name) \
    static void name(); \
    struct name##_reg { name##_reg() { tests.push_back({#name, name}); } } name##_inst; \
    static void name()

static std::vector<std::pair<std::string, void(*)()>> tests;

// ══════════════════════════════════════════════════════════════════════
// Tests
// ══════════════════════════════════════════════════════════════════════

TEST(test_basic_put_get) {
    dcs::cache::LRUCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    auto r = cache.get("a");
    assert(r.hit && r.value == "1");

    r = cache.get("b");
    assert(r.hit && r.value == "2");

    r = cache.get("c");
    assert(r.hit && r.value == "3");
}

TEST(test_cache_miss) {
    dcs::cache::LRUCache cache(2);
    auto r = cache.get("nonexistent");
    assert(!r.hit);
}

TEST(test_update_existing_key) {
    dcs::cache::LRUCache cache(2);
    cache.put("x", "old");
    cache.put("x", "new");

    auto r = cache.get("x");
    assert(r.hit && r.value == "new");
    assert(cache.size() == 1);
}

TEST(test_lru_eviction) {
    dcs::cache::LRUCache cache(3);
    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");

    // Cache is full. Inserting "d" should evict "a" (LRU).
    cache.put("d", "4");

    auto r = cache.get("a");
    assert(!r.hit);  // evicted

    r = cache.get("b");
    assert(r.hit && r.value == "2");

    r = cache.get("d");
    assert(r.hit && r.value == "4");
}

TEST(test_get_promotes_to_mru) {
    dcs::cache::LRUCache cache(3);
    cache.put("a", "1");  // LRU order: a
    cache.put("b", "2");  // LRU order: a, b
    cache.put("c", "3");  // LRU order: a, b, c

    // Access "a" — promotes it to MRU
    cache.get("a");        // LRU order: b, c, a

    // Insert "d" — should evict "b" (now the LRU), not "a"
    cache.put("d", "4");

    auto r = cache.get("b");
    assert(!r.hit);  // "b" was evicted

    r = cache.get("a");
    assert(r.hit);  // "a" is still here
}

TEST(test_delete) {
    dcs::cache::LRUCache cache(5);
    cache.put("x", "100");
    assert(cache.exists("x"));

    bool removed = cache.del("x");
    assert(removed);
    assert(!cache.exists("x"));
    assert(cache.size() == 0);

    // Delete non-existent key
    assert(!cache.del("nonexistent"));
}

TEST(test_keys) {
    dcs::cache::LRUCache cache(10);
    cache.put("alpha", "1");
    cache.put("beta", "2");
    cache.put("gamma", "3");

    auto k = cache.keys();
    assert(k.size() == 3);
}

TEST(test_dirty_tracking) {
    dcs::cache::LRUCache cache(10);
    cache.put("a", "1");
    cache.put("b", "2");

    auto dirty = cache.dirty_entries();
    assert(dirty.size() == 2);

    cache.clear_dirty("a");
    dirty = cache.dirty_entries();
    assert(dirty.size() == 1);
    assert(dirty[0].first == "b");
}

TEST(test_eviction_callback) {
    std::string evicted_key;
    std::string evicted_val;

    dcs::cache::LRUCache cache(2);
    cache.set_eviction_callback([&](const std::string& k, const std::string& v, bool) {
        evicted_key = k;
        evicted_val = v;
    });

    cache.put("a", "1");
    cache.put("b", "2");
    cache.put("c", "3");  // evicts "a"

    assert(evicted_key == "a");
    assert(evicted_val == "1");
}

TEST(test_capacity_one) {
    dcs::cache::LRUCache cache(1);
    cache.put("a", "1");
    cache.put("b", "2");  // evicts "a"

    assert(!cache.get("a").hit);
    assert(cache.get("b").hit);
    assert(cache.size() == 1);
}

TEST(test_large_values) {
    dcs::cache::LRUCache cache(2);
    std::string big(100000, 'X');
    cache.put("big", big);

    auto r = cache.get("big");
    assert(r.hit && r.value == big);
}

// ══════════════════════════════════════════════════════════════════════
// Runner
// ══════════════════════════════════════════════════════════════════════

int main() {
    int passed = 0, failed = 0;
    std::cout << "=== LRU Cache Tests ===\n\n";

    for (size_t i = 0; i < tests.size(); ++i) {
        try {
            tests[i].second();
            std::cout << "  [PASS] " << tests[i].first << "\n";
            ++passed;
        } catch (const std::exception& e) {
            std::cout << "  [FAIL] " << tests[i].first << ": " << e.what() << "\n";
            ++failed;
        } catch (...) {
            std::cout << "  [FAIL] " << tests[i].first << ": assertion failed\n";
            ++failed;
        }
    }

    std::cout << "\nResults: " << passed << " passed, " << failed << " failed.\n";
    return failed > 0 ? 1 : 0;
}
