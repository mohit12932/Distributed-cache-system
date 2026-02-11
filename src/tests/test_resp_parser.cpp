/**
 * Test suite for RESP protocol parser and client handler.
 */

#include "include/network/resp_parser.h"
#include "include/network/client_handler.h"
#include "include/sync/cache_manager.h"
#include "include/persistence/file_storage.h"

#include <iostream>
#include <cassert>
#include <string>
#include <vector>
#include <cstdlib>

#define TEST(name) \
    static void name(); \
    struct name##_reg { name##_reg() { tests.push_back({#name, name}); } } name##_inst; \
    static void name()

static std::vector<std::pair<std::string, void(*)()>> tests;

using namespace dcs::network;

// ══════════════════════════════════════════════════════════════════════
// RESP Parser Tests
// ══════════════════════════════════════════════════════════════════════

TEST(test_parse_inline_command) {
    size_t consumed = 0;
    auto tokens = RESPParser::parse("SET name Gemini\r\n", consumed);
    assert(tokens.size() == 3);
    assert(tokens[0] == "SET");
    assert(tokens[1] == "name");
    assert(tokens[2] == "Gemini");
    assert(consumed == 17);
}

TEST(test_parse_inline_get) {
    size_t consumed = 0;
    auto tokens = RESPParser::parse("GET name\r\n", consumed);
    assert(tokens.size() == 2);
    assert(tokens[0] == "GET");
    assert(tokens[1] == "name");
}

TEST(test_parse_resp_array) {
    // *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nGemini\r\n
    std::string msg = "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nGemini\r\n";
    size_t consumed = 0;
    auto tokens = RESPParser::parse(msg, consumed);
    assert(tokens.size() == 3);
    assert(tokens[0] == "SET");
    assert(tokens[1] == "name");
    assert(tokens[2] == "Gemini");
    assert(consumed == msg.size());
}

TEST(test_parse_resp_get) {
    std::string msg = "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n";
    size_t consumed = 0;
    auto tokens = RESPParser::parse(msg, consumed);
    assert(tokens.size() == 2);
    assert(tokens[0] == "GET");
    assert(tokens[1] == "name");
}

TEST(test_encode_simple_string) {
    assert(RESPParser::encode_simple_string("OK") == "+OK\r\n");
}

TEST(test_encode_error) {
    assert(RESPParser::encode_error("bad key") == "-ERR bad key\r\n");
}

TEST(test_encode_integer) {
    assert(RESPParser::encode_integer(42) == ":42\r\n");
}

TEST(test_encode_bulk_string) {
    assert(RESPParser::encode_bulk_string("hello") == "$5\r\nhello\r\n");
}

TEST(test_encode_null) {
    assert(RESPParser::encode_null() == "$-1\r\n");
}

TEST(test_encode_array) {
    auto result = RESPParser::encode_array({"a", "b"});
    assert(result == "*2\r\n$1\r\na\r\n$1\r\nb\r\n");
}

// ══════════════════════════════════════════════════════════════════════
// Client Handler Tests
// ══════════════════════════════════════════════════════════════════════

TEST(test_handler_set_get) {
    std::string test_file = "test_data/handler_test.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    cfg.write_mode = dcs::sync::WriteMode::WriteThrough;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    auto resp = handler.execute({"SET", "greeting", "hello"});
    assert(resp.data == "+OK\r\n");

    resp = handler.execute({"GET", "greeting"});
    assert(resp.data == "$5\r\nhello\r\n");

    // Cleanup
    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

TEST(test_handler_del) {
    std::string test_file = "test_data/handler_del.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    cfg.write_mode = dcs::sync::WriteMode::WriteThrough;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    handler.execute({"SET", "x", "1"});
    auto resp = handler.execute({"DEL", "x"});
    assert(resp.data == ":1\r\n");

    resp = handler.execute({"GET", "x"});
    assert(resp.data == "$-1\r\n");

    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

TEST(test_handler_exists) {
    std::string test_file = "test_data/handler_exists.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    cfg.write_mode = dcs::sync::WriteMode::WriteThrough;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    handler.execute({"SET", "y", "val"});
    auto resp = handler.execute({"EXISTS", "y"});
    assert(resp.data == ":1\r\n");

    resp = handler.execute({"EXISTS", "nope"});
    assert(resp.data == ":0\r\n");

    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

TEST(test_handler_ping) {
    std::string test_file = "test_data/handler_ping.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    auto resp = handler.execute({"PING"});
    assert(resp.data == "+PONG\r\n");

    resp = handler.execute({"PING", "hello"});
    assert(resp.data == "$5\r\nhello\r\n");

    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

TEST(test_handler_unknown_command) {
    std::string test_file = "test_data/handler_unknown.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    auto resp = handler.execute({"XYZZY"});
    assert(resp.data.find("-ERR") == 0);

    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

TEST(test_handler_quit) {
    std::string test_file = "test_data/handler_quit.dat";
    dcs::persistence::FileStorage storage(test_file);
    dcs::sync::CacheManager::Config cfg;
    dcs::sync::CacheManager manager(cfg, &storage);
    ClientHandler handler(&manager);

    auto resp = handler.execute({"QUIT"});
    assert(resp.data == "+OK\r\n");
    assert(resp.close_connection == true);

    manager.shutdown();
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
}

// ══════════════════════════════════════════════════════════════════════

int main() {
    int passed = 0, failed = 0;
    std::cout << "=== RESP Parser & Handler Tests ===\n\n";

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

    // Cleanup
#ifdef _WIN32
    system("rmdir /s /q test_data 2>nul");
#else
    system("rm -rf test_data");
#endif
    return failed > 0 ? 1 : 0;
}
