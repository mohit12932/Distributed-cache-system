#pragma once

#include "resp_parser.h"
#include "../sync/cache_manager.h"

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <iostream>

namespace dcs {
namespace network {

/**
 * ClientHandler — Processes parsed RESP commands against the CacheManager.
 *
 * Supported commands:
 *   GET <key>                -> Bulk string or Null
 *   SET <key> <value>        -> +OK
 *   DEL <key> [key ...]      -> :<count>
 *   EXISTS <key>             -> :0 or :1
 *   KEYS *                   -> Array of bulk strings
 *   DBSIZE                   -> :<count>
 *   FLUSHALL                 -> +OK
 *   PING [message]           -> +PONG or bulk string
 *   INFO                     -> Bulk string with stats
 *   COMMAND                  -> +OK  (stub for redis-cli handshake)
 *   QUIT                     -> +OK  (signals disconnect)
 *   CONFIG GET <param>       -> Array (stub for redis-cli compat)
 */
class ClientHandler {
public:
    explicit ClientHandler(sync::CacheManager* manager)
        : manager_(manager) {}

    struct Response {
        std::string data;
        bool close_connection = false;
    };

    /**
     * Execute a single tokenized command and return the RESP-encoded response.
     */
    Response execute(const std::vector<std::string>& tokens) {
        if (tokens.empty()) {
            return {RESPParser::encode_error("empty command")};
        }

        std::string cmd = to_upper(tokens[0]);

        // ── Core Data Commands ───────────────────────────────────
        if (cmd == "GET") {
            if (tokens.size() < 2) return {RESPParser::encode_error("wrong number of arguments for 'GET'")};
            auto result = manager_->get(tokens[1]);
            if (result.hit) return {RESPParser::encode_bulk_string(result.value)};
            return {RESPParser::encode_null()};
        }

        if (cmd == "SET") {
            if (tokens.size() < 3) return {RESPParser::encode_error("wrong number of arguments for 'SET'")};
            // Concatenate remaining tokens for values with spaces (inline mode)
            std::string value = tokens[2];
            for (size_t i = 3; i < tokens.size(); ++i) {
                value += " " + tokens[i];
            }
            manager_->put(tokens[1], value);
            return {RESPParser::encode_simple_string("OK")};
        }

        if (cmd == "DEL") {
            if (tokens.size() < 2) return {RESPParser::encode_error("wrong number of arguments for 'DEL'")};
            int64_t count = 0;
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (manager_->del(tokens[i])) ++count;
            }
            return {RESPParser::encode_integer(count)};
        }

        if (cmd == "EXISTS") {
            if (tokens.size() < 2) return {RESPParser::encode_error("wrong number of arguments for 'EXISTS'")};
            return {RESPParser::encode_integer(manager_->exists(tokens[1]) ? 1 : 0)};
        }

        if (cmd == "KEYS") {
            auto all_keys = manager_->keys();
            return {RESPParser::encode_array(all_keys)};
        }

        if (cmd == "DBSIZE") {
            return {RESPParser::encode_integer(static_cast<int64_t>(manager_->size()))};
        }

        // ── Admin Commands ───────────────────────────────────────
        if (cmd == "FLUSHALL" || cmd == "FLUSHDB") {
            manager_->flush_all();
            return {RESPParser::encode_simple_string("OK")};
        }

        if (cmd == "PING") {
            if (tokens.size() >= 2) return {RESPParser::encode_bulk_string(tokens[1])};
            return {RESPParser::encode_simple_string("PONG")};
        }

        if (cmd == "QUIT") {
            return {RESPParser::encode_simple_string("OK"), true};
        }

        if (cmd == "INFO") {
            return {RESPParser::encode_bulk_string(build_info())};
        }

        // ── redis-cli compatibility stubs ────────────────────────
        if (cmd == "COMMAND") {
            // redis-cli sends "COMMAND DOCS" on connect — just return empty array
            return {RESPParser::encode_simple_string("OK")};
        }

        if (cmd == "CONFIG") {
            // redis-cli sends CONFIG GET save, CONFIG GET appendonly, etc.
            if (tokens.size() >= 3 && to_upper(tokens[1]) == "GET") {
                // Return empty array (no matching config)
                return {"*2\r\n" + RESPParser::encode_bulk_string(tokens[2])
                        + RESPParser::encode_bulk_string("")};
            }
            return {RESPParser::encode_simple_string("OK")};
        }

        if (cmd == "CLIENT") {
            // redis-cli sends CLIENT SETNAME, CLIENT GETNAME, etc.
            return {RESPParser::encode_simple_string("OK")};
        }

        // ── Unknown command ──────────────────────────────────────
        return {RESPParser::encode_error("unknown command '" + tokens[0] + "'")};
    }

private:
    static std::string to_upper(const std::string& s) {
        std::string result = s;
        std::transform(result.begin(), result.end(), result.begin(),
                       [](unsigned char c) { return std::toupper(c); });
        return result;
    }

    std::string build_info() const {
        auto& s = manager_->stats();
        std::string mode = (manager_->write_mode() == sync::WriteMode::WriteThrough)
                           ? "write-through" : "write-back";
        std::string info;
        info += "# Server\r\n";
        info += "distributed_cache_version:1.0.0\r\n";
        info += "write_mode:" + mode + "\r\n";
        info += "\r\n# Stats\r\n";
        info += "cache_hits:" + std::to_string(s.cache_hits.load()) + "\r\n";
        info += "cache_misses:" + std::to_string(s.cache_misses.load()) + "\r\n";
        info += "write_through_ops:" + std::to_string(s.write_through_count.load()) + "\r\n";
        info += "write_back_ops:" + std::to_string(s.write_back_count.load()) + "\r\n";
        info += "\r\n# Keyspace\r\n";
        info += "keys:" + std::to_string(manager_->size()) + "\r\n";
        return info;
    }

    sync::CacheManager* manager_;
};

}  // namespace network
}  // namespace dcs
