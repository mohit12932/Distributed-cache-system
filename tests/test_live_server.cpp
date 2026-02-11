/**
 * Live server integration test — connects over TCP and tests all commands.
 * Run the server first: distributed_cache.exe --port 6399
 * Then run this test:  test_live_server.exe
 */

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    using socket_t = SOCKET;
    #define SOCKET_INVALID INVALID_SOCKET
    #define CLOSE_SOCKET(s) closesocket(s)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using socket_t = int;
    #define SOCKET_INVALID (-1)
    #define CLOSE_SOCKET(s) close(s)
#endif

#include <iostream>
#include <string>
#include <cstring>
#include <cassert>
#include <vector>
#include <chrono>
// ── Helpers ───────────────────────────────────────────────────────────

socket_t connect_to_server(const char* host, uint16_t port) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == SOCKET_INVALID) return SOCKET_INVALID;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) != 0) {
        CLOSE_SOCKET(sock);
        return SOCKET_INVALID;
    }
    return sock;
}

std::string send_command(socket_t sock, const std::string& cmd) {
    std::string full = cmd + "\r\n";
    send(sock, full.c_str(), (int)full.size(), 0);

    // Small delay for server to process
#ifdef _WIN32
    Sleep(50);
#endif

    char buf[4096] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return "(disconnected)";
    return std::string(buf, n);
}

// ── Tests ─────────────────────────────────────────────────────────────

int main() {
    std::cout << "========================================\n";
    std::cout << "  TEST SUITE 4: Live Server Integration \n";
    std::cout << "========================================\n\n";

    socket_t sock = connect_to_server("127.0.0.1", 6399);
    if (sock == SOCKET_INVALID) {
        std::cerr << "[ERROR] Cannot connect to server on port 6399.\n";
        std::cerr << "        Start the server first with: distributed_cache.exe --port 6399\n";
        return 1;
    }

    int passed = 0, failed = 0;
    auto check = [&](const std::string& name, const std::string& got, 
                      const std::string& expected) {
        // Trim trailing whitespace for comparison
        std::string g = got, e = expected;
        while (!g.empty() && (g.back() == '\r' || g.back() == '\n')) g.pop_back();
        while (!e.empty() && (e.back() == '\r' || e.back() == '\n')) e.pop_back();
        if (g == e) {
            std::cout << "  [PASS] " << name << "\n";
            passed++;
        } else {
            std::cout << "  [FAIL] " << name << "\n";
            std::cout << "         Expected: \"" << expected << "\"\n";
            std::cout << "         Got:      \"" << got << "\"\n";
            failed++;
        }
    };

    auto contains = [&](const std::string& name, const std::string& got,
                         const std::string& substr) {
        if (got.find(substr) != std::string::npos) {
            std::cout << "  [PASS] " << name << "\n";
            passed++;
        } else {
            std::cout << "  [FAIL] " << name << "\n";
            std::cout << "         Expected to contain: \"" << substr << "\"\n";
            std::cout << "         Got: \"" << got << "\"\n";
            failed++;
        }
    };

    // ── 1. PING ──────────────────────────────────────────────────
    std::cout << "--- PING Command ---\n";
    check("PING returns PONG", send_command(sock, "PING"), "+PONG\r\n");
    check("PING with message", send_command(sock, "PING hello"), "$5\r\nhello\r\n");

    // ── 2. SET / GET ─────────────────────────────────────────────
    std::cout << "\n--- SET / GET Commands ---\n";
    check("SET key returns OK", send_command(sock, "SET name Alice"), "+OK\r\n");
    check("GET existing key", send_command(sock, "GET name"), "$5\r\nAlice\r\n");
    check("SET another key", send_command(sock, "SET city NewYork"), "+OK\r\n");
    check("GET another key", send_command(sock, "GET city"), "$7\r\nNewYork\r\n");
    check("GET missing key returns nil", send_command(sock, "GET nonexistent"), "$-1\r\n");

    // ── 3. UPDATE ────────────────────────────────────────────────
    std::cout << "\n--- UPDATE Existing Key ---\n";
    check("SET overwrites value", send_command(sock, "SET name Bob"), "+OK\r\n");
    check("GET returns updated value", send_command(sock, "GET name"), "$3\r\nBob\r\n");

    // ── 4. EXISTS ────────────────────────────────────────────────
    std::cout << "\n--- EXISTS Command ---\n";
    check("EXISTS on present key", send_command(sock, "EXISTS name"), ":1\r\n");
    check("EXISTS on missing key", send_command(sock, "EXISTS ghost"), ":0\r\n");

    // ── 5. DEL ───────────────────────────────────────────────────
    std::cout << "\n--- DEL Command ---\n";
    check("DEL existing key", send_command(sock, "DEL city"), ":1\r\n");
    check("GET deleted key is nil", send_command(sock, "GET city"), "$-1\r\n");
    check("DEL non-existing key", send_command(sock, "DEL ghost"), ":1\r\n");

    // ── 6. Multiple keys ────────────────────────────────────────
    std::cout << "\n--- Bulk Operations ---\n";
    send_command(sock, "SET k1 v1");
    send_command(sock, "SET k2 v2");
    send_command(sock, "SET k3 v3");
    auto dbsize_resp = send_command(sock, "DBSIZE");
    contains("DBSIZE returns integer", dbsize_resp, ":");

    // ── 7. KEYS ──────────────────────────────────────────────────
    std::cout << "\n--- KEYS Command ---\n";
    auto keys_resp = send_command(sock, "KEYS *");
    contains("KEYS returns array", keys_resp, "*");
    contains("KEYS contains name", keys_resp, "name");

    // ── 8. INFO ──────────────────────────────────────────────────
    std::cout << "\n--- INFO Command ---\n";
    auto info_resp = send_command(sock, "INFO");
    contains("INFO has version", info_resp, "distributed_cache_version:1.0.0");
    contains("INFO has write_mode", info_resp, "write_mode:write-through");
    contains("INFO has cache_hits", info_resp, "cache_hits:");

    // ── 9. FLUSHALL ──────────────────────────────────────────────
    std::cout << "\n--- FLUSHALL Command ---\n";
    check("FLUSHALL returns OK", send_command(sock, "FLUSHALL"), "+OK\r\n");
    check("DBSIZE is 0 after flush", send_command(sock, "DBSIZE"), ":0\r\n");

    // ── 10. Persistence test ─────────────────────────────────────
    std::cout << "\n--- Persistence (Write-Through) ---\n";
    check("SET persisted key", send_command(sock, "SET persist_key persist_val"), "+OK\r\n");
    check("GET persisted key", send_command(sock, "GET persist_key"), "$11\r\npersist_val\r\n");

    // ── 11. Error handling ───────────────────────────────────────
    std::cout << "\n--- Error Handling ---\n";
    contains("Unknown command error", send_command(sock, "BADCMD"), "-ERR");
    contains("GET wrong args error", send_command(sock, "GET"), "-ERR");

    // ── Summary ──────────────────────────────────────────────────
    std::cout << "\n========================================\n";
    std::cout << "Results: " << passed << " passed, " << failed << " failed.\n";
    std::cout << "========================================\n";

    CLOSE_SOCKET(sock);
#ifdef _WIN32
    WSACleanup();
#endif

    return failed > 0 ? 1 : 0;
}
