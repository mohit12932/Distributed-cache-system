#pragma once

#include <string>
#include <vector>
#include <sstream>
#include <stdexcept>

namespace dcs {
namespace network {

/**
 * RESP (REdis Serialization Protocol) Parser & Encoder.
 *
 * Supports parsing RESP2 inline commands and array/bulk-string protocol,
 * making this server compatible with redis-cli and any Redis client library.
 *
 * RESP types handled:
 *   +  Simple String    "+OK\r\n"
 *   -  Error            "-ERR message\r\n"
 *   :  Integer          ":42\r\n"
 *   $  Bulk String      "$5\r\nhello\r\n"  or  "$-1\r\n" (null)
 *   *  Array            "*2\r\n$3\r\nGET\r\n$4\r\nname\r\n"
 */
class RESPParser {
public:
    // ── Encoding helpers (server -> client) ─────────────────────────

    static std::string encode_simple_string(const std::string& s) {
        return "+" + s + "\r\n";
    }

    static std::string encode_error(const std::string& msg) {
        return "-ERR " + msg + "\r\n";
    }

    static std::string encode_integer(int64_t n) {
        return ":" + std::to_string(n) + "\r\n";
    }

    static std::string encode_bulk_string(const std::string& s) {
        return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
    }

    static std::string encode_null() {
        return "$-1\r\n";
    }

    static std::string encode_array(const std::vector<std::string>& items) {
        std::string out = "*" + std::to_string(items.size()) + "\r\n";
        for (auto& item : items) {
            out += encode_bulk_string(item);
        }
        return out;
    }

    // ── Decoding (client -> server) ─────────────────────────────────

    /**
     * Parse a complete RESP message from a buffer.
     * Returns the parsed tokens (e.g. ["SET", "name", "Gemini"]).
     * Sets `bytes_consumed` to how many bytes of `buf` were used.
     *
     * Supports:
     *   - RESP arrays (*N\r\n...) from proper Redis clients.
     *   - Inline commands ("SET name Gemini\r\n") from telnet / redis-cli inline.
     */
    static std::vector<std::string> parse(const std::string& buf, size_t& bytes_consumed) {
        bytes_consumed = 0;
        if (buf.empty()) return {};

        if (buf[0] == '*') {
            return parse_array(buf, bytes_consumed);
        } else {
            return parse_inline(buf, bytes_consumed);
        }
    }

private:
    /** Parse a RESP array: *N\r\n followed by N bulk strings. */
    static std::vector<std::string> parse_array(const std::string& buf, size_t& consumed) {
        size_t pos = 1;  // skip '*'
        auto crlf = buf.find("\r\n", pos);
        if (crlf == std::string::npos) return {};

        int count = std::stoi(buf.substr(pos, crlf - pos));
        pos = crlf + 2;

        std::vector<std::string> tokens;
        for (int i = 0; i < count; ++i) {
            if (pos >= buf.size()) return {};  // incomplete

            if (buf[pos] != '$') {
                // Unexpected type — skip
                auto next_crlf = buf.find("\r\n", pos);
                if (next_crlf == std::string::npos) return {};
                tokens.push_back(buf.substr(pos + 1, next_crlf - pos - 1));
                pos = next_crlf + 2;
                continue;
            }

            // Bulk string: $N\r\n<N bytes>\r\n
            size_t dollar = pos + 1;
            auto len_end = buf.find("\r\n", dollar);
            if (len_end == std::string::npos) return {};

            int len = std::stoi(buf.substr(dollar, len_end - dollar));
            if (len < 0) {
                tokens.emplace_back("");  // null bulk string
                pos = len_end + 2;
                continue;
            }

            size_t data_start = len_end + 2;
            if (data_start + len + 2 > buf.size()) return {};  // incomplete

            tokens.push_back(buf.substr(data_start, len));
            pos = data_start + len + 2;  // skip trailing \r\n
        }

        consumed = pos;
        return tokens;
    }

    /** Parse an inline command: "SET name Gemini\r\n" */
    static std::vector<std::string> parse_inline(const std::string& buf, size_t& consumed) {
        auto crlf = buf.find("\r\n");
        std::string line;
        if (crlf != std::string::npos) {
            line = buf.substr(0, crlf);
            consumed = crlf + 2;
        } else {
            // Also accept LF-only or unterminated (for telnet)
            auto lf = buf.find('\n');
            if (lf != std::string::npos) {
                line = buf.substr(0, lf);
                consumed = lf + 1;
            } else {
                line = buf;
                consumed = buf.size();
            }
        }

        // Tokenize by whitespace
        std::vector<std::string> tokens;
        std::istringstream iss(line);
        std::string token;
        while (iss >> token) {
            tokens.push_back(token);
        }
        return tokens;
    }
};

}  // namespace network
}  // namespace dcs
