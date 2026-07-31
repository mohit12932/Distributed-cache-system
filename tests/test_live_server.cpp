/**
 * Live server integration tests.
 *
 * This suite talks to a running distributed_cache process over TCP RESP
 * and HTTP to verify the end-to-end server path used in CI.
 */

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    using socket_t = SOCKET;
    static constexpr socket_t INVALID_SOCKET_FD = INVALID_SOCKET;
    static void close_socket(socket_t sock) { closesocket(sock); }
    static void sleep_ms(int ms) { Sleep(static_cast<DWORD>(ms)); }
#else
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <unistd.h>
    using socket_t = int;
    static constexpr socket_t INVALID_SOCKET_FD = -1;
    static void close_socket(socket_t sock) { close(sock); }
    static void sleep_ms(int ms) { usleep(static_cast<useconds_t>(ms) * 1000); }
#endif

struct SocketInit {
    SocketInit() {
#ifdef _WIN32
        WSADATA data{};
        assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
#endif
    }

    ~SocketInit() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

static int env_int(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (!value || !*value) return fallback;
    return std::atoi(value);
}

static bool set_ipv4_address(sockaddr_in& addr, const std::string& host) {
    unsigned long parsed = inet_addr(host.c_str());
    if (parsed == INADDR_NONE && host != "255.255.255.255") {
        return false;
    }
    addr.sin_addr.s_addr = parsed;
    return true;
}

static socket_t connect_tcp(const std::string& host, int port) {
    socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET_FD) return INVALID_SOCKET_FD;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (!set_ipv4_address(addr, host)) {
        close_socket(sock);
        return INVALID_SOCKET_FD;
    }

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(sock);
        return INVALID_SOCKET_FD;
    }
    return sock;
}

static bool send_all(socket_t sock, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        int n = send(sock, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        ssize_t n = send(sock, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

static std::string recv_all(socket_t sock) {
    std::string out;
    char buf[4096];
    while (true) {
#ifdef _WIN32
        int n = recv(sock, buf, sizeof(buf), 0);
#else
        ssize_t n = recv(sock, buf, sizeof(buf), 0);
#endif
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
        if (n < static_cast<int>(sizeof(buf))) break;
    }
    return out;
}

static std::string http_get(int port, const std::string& path) {
    socket_t sock = connect_tcp("127.0.0.1", port);
    assert(sock != INVALID_SOCKET_FD);

    std::ostringstream req;
    req << "GET " << path << " HTTP/1.1\r\n"
        << "Host: 127.0.0.1\r\n"
        << "Connection: close\r\n\r\n";
    assert(send_all(sock, req.str()));

    std::string response = recv_all(sock);
    close_socket(sock);
    return response;
}

static std::string resp_command(int port, const std::string& command) {
    socket_t sock = connect_tcp("127.0.0.1", port);
    assert(sock != INVALID_SOCKET_FD);
    assert(send_all(sock, command));

    std::string response = recv_all(sock);
    close_socket(sock);
    return response;
}

static void wait_for_port(int port) {
    for (int i = 0; i < 50; ++i) {
        socket_t sock = connect_tcp("127.0.0.1", port);
        if (sock != INVALID_SOCKET_FD) {
            close_socket(sock);
            return;
        }
        sleep_ms(200);
    }
    assert(false && "server did not start on time");
}

static void test_tcp_ping(int port) {
    auto response = resp_command(port, "PING\r\n");
    assert(response == "+PONG\r\n");
}

static void test_tcp_set_get(int port) {
    auto set_response = resp_command(port, "SET live:test hello\r\n");
    assert(set_response == "+OK\r\n");

    auto get_response = resp_command(port, "GET live:test\r\n");
    assert(get_response == "$5\r\nhello\r\n");

    auto exists_response = resp_command(port, "EXISTS live:test\r\n");
    assert(exists_response == ":1\r\n");
}

static void test_tcp_info(int port) {
    auto response = resp_command(port, "INFO\r\n");
    assert(response.find("# Server") != std::string::npos);
    assert(response.find("write_mode:write-through") != std::string::npos);
}

static void test_http_metrics(int port) {
    auto response = http_get(port, "/metrics");
    assert(response.find("HTTP/1.1 200 OK") != std::string::npos);
    assert(response.find("\"write_mode\": \"write-through\"") != std::string::npos);
    assert(response.find("\"cache_hits\"") != std::string::npos);
}

int main() {
    SocketInit socket_init;

    const int tcp_port = env_int("TCP_PORT", 6399);
    const int http_port = env_int("HTTP_PORT", 8080);

    std::cout << "=== Live Server Integration Tests ===\n\n";
    wait_for_port(tcp_port);
    wait_for_port(http_port);

    test_tcp_ping(tcp_port);
    std::cout << "  [PASS] TCP PING\n";

    test_tcp_set_get(tcp_port);
    std::cout << "  [PASS] TCP SET/GET/EXISTS\n";

    test_tcp_info(tcp_port);
    std::cout << "  [PASS] TCP INFO\n";

    test_http_metrics(http_port);
    std::cout << "  [PASS] HTTP /metrics\n";

    std::cout << "\nResults: 4 passed, 0 failed.\n";
    return 0;
}