#pragma once

// Winsock MUST be included before windows.h (pulled in by compat/threading.h)
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

#include "client_handler.h"
#include "resp_parser.h"
#include "../sync/cache_manager.h"
#include "../compat/threading.h"

#include <string>
#include <vector>
#include <iostream>
#include <functional>
#include <cstring>

namespace dcs {
namespace network {

/**
 * TCPServer — Multi-threaded TCP server speaking the RESP protocol.
 *
 * Compatible with redis-cli, any Redis client library, or plain telnet.
 * One thread per client (simple model; could be upgraded to epoll/io_uring).
 *
 * Usage:
 *   TCPServer server(6379, &cache_manager);
 *   server.start();   // blocks in accept loop
 *   // ... from another thread: server.stop();
 */
class TCPServer {
public:
    TCPServer(uint16_t port, sync::CacheManager* manager)
        : port_(port)
        , manager_(manager)
        , running_(false)
        , listen_fd_(SOCKET_INVALID)
        , client_count_(0) {}

    ~TCPServer() {
        stop();
    }

    /** Start the server (blocking — call from dedicated thread or main). */
    void start() {
        if (!init_socket()) return;
        running_ = true;

        std::cout << "=== Distributed Cache Server ===\n";
        std::cout << "Listening on port " << port_ << "\n";
        std::cout << "Compatible with redis-cli: redis-cli -p " << port_ << "\n";
        std::cout << "Press Ctrl+C to stop.\n\n";

        accept_loop();
    }

    /** Signal the server to stop accepting new connections. */
    void stop() {
        running_ = false;
        if (listen_fd_ != SOCKET_INVALID) {
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = SOCKET_INVALID;
        }

        // Join all client threads
        compat::LockGuard<compat::Mutex> lock(threads_mu_);
        for (auto& t : client_threads_) {
            if (t.joinable()) t.join();
        }
        client_threads_.clear();

#ifdef _WIN32
        WSACleanup();
#endif
    }

    uint32_t client_count() const { return client_count_.load(); }

private:
    bool init_socket() {
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            std::cerr << "[TCP] WSAStartup failed\n";
            return false;
        }
#endif

        listen_fd_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_fd_ == SOCKET_INVALID) {
            std::cerr << "[TCP] socket() failed\n";
            return false;
        }

        // Allow port reuse
        int opt = 1;
#ifdef _WIN32
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            std::cerr << "[TCP] bind() failed on port " << port_ << "\n";
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = SOCKET_INVALID;
            return false;
        }

        if (listen(listen_fd_, SOMAXCONN) != 0) {
            std::cerr << "[TCP] listen() failed\n";
            CLOSE_SOCKET(listen_fd_);
            listen_fd_ = SOCKET_INVALID;
            return false;
        }

        return true;
    }

    void accept_loop() {
        while (running_) {
            sockaddr_in client_addr{};
#ifdef _WIN32
            int addr_len = sizeof(client_addr);
#else
            socklen_t addr_len = sizeof(client_addr);
#endif
            socket_t client_fd = accept(listen_fd_,
                                        reinterpret_cast<sockaddr*>(&client_addr),
                                        &addr_len);
            if (client_fd == SOCKET_INVALID) {
                if (running_) {
                    std::cerr << "[TCP] accept() failed\n";
                }
                continue;
            }

            client_count_++;
            std::string ip = inet_ntoa(client_addr.sin_addr);
            std::cout << "[TCP] Client connected: " << ip
                      << ":" << ntohs(client_addr.sin_port) << "\n";

            // Spawn a thread for this client
            compat::LockGuard<compat::Mutex> lock(threads_mu_);
            client_threads_.push_back(compat::Thread([this, client_fd, ip]() {
                handle_client(client_fd, ip);
            }));
        }
    }

    void handle_client(socket_t fd, std::string ip) {
        ClientHandler handler(manager_);
        std::string buffer;
        char recv_buf[4096];

        while (running_) {
#ifdef _WIN32
            int n = recv(fd, recv_buf, sizeof(recv_buf), 0);
#else
            ssize_t n = recv(fd, recv_buf, sizeof(recv_buf), 0);
#endif
            if (n <= 0) break;  // client disconnected or error

            buffer.append(recv_buf, n);

            // Process all complete commands in the buffer
            while (!buffer.empty()) {
                size_t consumed = 0;
                auto tokens = RESPParser::parse(buffer, consumed);

                if (tokens.empty() || consumed == 0) break;  // need more data

                auto response = handler.execute(tokens);

                // Send response
                send_all(fd, response.data);

                // Remove consumed bytes
                buffer.erase(0, consumed);

                if (response.close_connection) {
                    CLOSE_SOCKET(fd);
                    std::cout << "[TCP] Client disconnected (QUIT): " << ip << "\n";
                    return;
                }
            }
        }

        CLOSE_SOCKET(fd);
        std::cout << "[TCP] Client disconnected: " << ip << "\n";
    }

    void send_all(socket_t fd, const std::string& data) {
        size_t total_sent = 0;
        while (total_sent < data.size()) {
#ifdef _WIN32
            int n = send(fd, data.c_str() + total_sent,
                         static_cast<int>(data.size() - total_sent), 0);
#else
            ssize_t n = send(fd, data.c_str() + total_sent,
                             data.size() - total_sent, 0);
#endif
            if (n <= 0) break;
            total_sent += n;
        }
    }

    uint16_t port_;
    sync::CacheManager* manager_;
    compat::Atomic<bool> running_;
    socket_t listen_fd_;
    compat::Atomic<uint32_t> client_count_;
    std::vector<compat::Thread> client_threads_;
    compat::Mutex threads_mu_;
};

}  // namespace network
}  // namespace dcs
