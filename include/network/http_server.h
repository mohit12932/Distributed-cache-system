#pragma once
// ────────────────────────────────────────────────────────────────
// HTTP Server: Minimal embedded HTTP server for serving the
// dashboard and JSON metrics API. Replaces the PowerShell script.
// Runs on port 8080 alongside the RESP TCP server on 6379.
// ────────────────────────────────────────────────────────────────

#ifdef _WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #pragma comment(lib, "Ws2_32.lib")
    using http_socket_t = SOCKET;
    #define HTTP_SOCKET_INVALID INVALID_SOCKET
    #define HTTP_CLOSE_SOCKET(s) closesocket(s)
#else
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    using http_socket_t = int;
    #define HTTP_SOCKET_INVALID (-1)
    #define HTTP_CLOSE_SOCKET(s) close(s)
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "../compat/threading.h"

namespace dcs {
namespace network {

class HTTPServer {
public:
    using MetricsCallback = std::function<std::string()>;

    HTTPServer(int port, const std::string& web_root)
        : port_(port), web_root_(web_root), running_(false),
          listen_sock_(HTTP_SOCKET_INVALID) {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }

    ~HTTPServer() { stop(); }

    void setMetricsCallback(MetricsCallback cb) { metrics_cb_ = std::move(cb); }

    // Register custom API endpoints: path → handler returning JSON
    void addEndpoint(const std::string& path,
                     std::function<std::string(const std::string&)> handler) {
        compat::LockGuard<compat::Mutex> lock(mu_);
        endpoints_[path] = std::move(handler);
    }

    void start() {
        running_ = true;
        accept_thread_ = compat::Thread(&HTTPServer::acceptLoop, this);
    }

    void stop() {
        running_ = false;
        if (listen_sock_ != HTTP_SOCKET_INVALID) {
            HTTP_CLOSE_SOCKET(listen_sock_);
            listen_sock_ = HTTP_SOCKET_INVALID;
        }
        if (accept_thread_.joinable()) accept_thread_.join();
    }

    bool isRunning() const { return running_; }

private:
    void logToFile(const std::string& msg) const {
        std::ofstream log("http_server_log.txt", std::ios::app);
        if (log.is_open()) {
            log << msg << "\n";
            log.flush();
        }
    }

    void acceptLoop() {
        logToFile("[HTTP] acceptLoop() started");
        listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listen_sock_ == HTTP_SOCKET_INVALID) {
            std::cerr << "[HTTP] Failed to create socket\n";
            logToFile("[HTTP] FAILED to create socket");
            return;
        }
        logToFile("[HTTP] socket created successfully");

        // Allow address reuse
        int opt = 1;
#ifdef _WIN32
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
        setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
        logToFile("[HTTP] SO_REUSEADDR set");

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(static_cast<uint16_t>(port_));

        if (bind(listen_sock_, reinterpret_cast<struct sockaddr*>(&addr),
                 sizeof(addr)) != 0) {
            std::cerr << "[HTTP] Bind failed on port " << port_ << "\n";
            logToFile("[HTTP] FAILED to bind on port " + std::to_string(port_));
            HTTP_CLOSE_SOCKET(listen_sock_);
            listen_sock_ = HTTP_SOCKET_INVALID;
            return;
        }
        logToFile("[HTTP] bind() successful on port " + std::to_string(port_));

        if (listen(listen_sock_, 16) != 0) {
            std::cerr << "[HTTP] Listen failed\n";
            logToFile("[HTTP] FAILED listen()");
            HTTP_CLOSE_SOCKET(listen_sock_);
            listen_sock_ = HTTP_SOCKET_INVALID;
            return;
        }
        logToFile("[HTTP] listen() successful");

        std::cout << "[HTTP] Dashboard server listening on http://localhost:"
                  << port_ << "\n";
        logToFile("[HTTP] Dashboard server listening on http://localhost:" + std::to_string(port_));

        while (running_) {
            struct sockaddr_in client_addr;
            int addr_len = sizeof(client_addr);
#ifdef _WIN32
            http_socket_t client = accept(listen_sock_,
                reinterpret_cast<struct sockaddr*>(&client_addr), &addr_len);
#else
            http_socket_t client = accept(listen_sock_,
                reinterpret_cast<struct sockaddr*>(&client_addr),
                reinterpret_cast<socklen_t*>(&addr_len));
#endif
            if (client == HTTP_SOCKET_INVALID) continue;

            // Handle each request in a detached thread
            compat::Thread([this, client]() {
                handleClient(client);
            }).detach();
        }
    }

    void handleClient(http_socket_t sock) {
        // Set socket timeout to prevent hung threads (5 seconds)
#ifdef _WIN32
        DWORD timeout_ms = 5000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout_ms, sizeof(timeout_ms));
#else
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
        char buf[8192];
        int n = recv(sock, buf, sizeof(buf) - 1, 0);
        if (n <= 0) {
            HTTP_CLOSE_SOCKET(sock);
            return;
        }
        buf[n] = '\0';
        std::string request(buf, n);

        // If we have Content-Length, make sure we read the entire body
        size_t cl_pos = request.find("Content-Length:");
        if (cl_pos == std::string::npos) cl_pos = request.find("content-length:");
        if (cl_pos != std::string::npos) {
            int content_len = std::atoi(request.c_str() + cl_pos + 15);
            size_t header_end = request.find("\r\n\r\n");
            if (header_end != std::string::npos) {
                size_t body_received = request.size() - (header_end + 4);
                int tries = 0;
                while ((int)body_received < content_len && tries < 50) {
                    int r = recv(sock, buf, std::min((int)sizeof(buf)-1, content_len - (int)body_received), 0);
                    if (r <= 0) break;
                    buf[r] = '\0';
                    request.append(buf, r);
                    body_received += r;
                    tries++;
                }
            }
        }
        std::string method, path;
        parseRequestLine(request, method, path);

        // CORS headers for dashboard
        std::string cors_headers =
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
            "Access-Control-Allow-Headers: Content-Type\r\n";

        if (method == "OPTIONS") {
            std::string response =
                "HTTP/1.1 204 No Content\r\n" + cors_headers +
                "Content-Length: 0\r\n\r\n";
            sendAll(sock, response);
            HTTP_CLOSE_SOCKET(sock);
            return;
        }

        // Route request
        if (path == "/metrics" || path == "/api/metrics") {
            serveMetrics(sock, cors_headers);
        } else if (path == "/api/start") {
            serveJSON(sock, R"({"status":"running"})", cors_headers);
        } else if (path == "/api/stop") {
            serveJSON(sock, R"({"status":"stopped"})", cors_headers);
        } else if (path == "/api/reset") {
            serveJSON(sock, R"({"status":"reset"})", cors_headers);
        } else {
            // Check custom endpoints
            bool handled = false;
            {
                // Copy handler out so we don't hold mu_ during execution
                std::function<std::string(const std::string&)> handler;
                {
                    compat::LockGuard<compat::Mutex> lock(mu_);
                    auto it = endpoints_.find(path);
                    if (it != endpoints_.end()) {
                        handler = it->second;
                    }
                }
                if (handler) {
                    std::string body = getRequestBody(request);
                    std::string json = handler(body);
                    serveJSON(sock, json, cors_headers);
                    handled = true;
                }
            }
            if (!handled) {
                serveFile(sock, path, cors_headers);
            }
        }
        HTTP_CLOSE_SOCKET(sock);
    }

    void serveMetrics(http_socket_t sock, const std::string& cors) {
        std::string json = "{}";
        if (metrics_cb_) json = metrics_cb_();
        serveJSON(sock, json, cors);
    }

    void serveJSON(http_socket_t sock, const std::string& json,
                   const std::string& cors) {
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n" +
            cors +
            "Content-Length: " + std::to_string(json.size()) + "\r\n"
            "Connection: close\r\n\r\n" + json;
        sendAll(sock, response);
    }

    void serveFile(http_socket_t sock, const std::string& url_path,
                   const std::string& cors) {
        std::string file_path = url_path;
        if (file_path == "/") file_path = "/dashboard.html";

        // Security: prevent path traversal
        if (file_path.find("..") != std::string::npos) {
            serve404(sock, cors);
            return;
        }

        std::string full_path = web_root_ + file_path;
        // Convert forward slashes to backslashes on Windows
#ifdef _WIN32
        for (char& c : full_path) {
            if (c == '/') c = '\\';
        }
#endif

        std::ifstream file(full_path, std::ios::binary);
        if (!file.is_open()) {
            serve404(sock, cors);
            return;
        }

        std::ostringstream ss;
        ss << file.rdbuf();
        std::string content = ss.str();

        std::string content_type = guessContentType(file_path);
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + content_type + "\r\n" +
            cors +
            "Content-Length: " + std::to_string(content.size()) + "\r\n"
            "Connection: close\r\n\r\n" + content;
        sendAll(sock, response);
    }

    void serve404(http_socket_t sock, const std::string& cors) {
        std::string body = "<html><body><h1>404 Not Found</h1></body></html>";
        std::string response =
            "HTTP/1.1 404 Not Found\r\n"
            "Content-Type: text/html\r\n" +
            cors +
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + body;
        sendAll(sock, response);
    }

    static void parseRequestLine(const std::string& request,
                                  std::string& method, std::string& path) {
        size_t space1 = request.find(' ');
        if (space1 == std::string::npos) { method = "GET"; path = "/"; return; }
        method = request.substr(0, space1);

        size_t space2 = request.find(' ', space1 + 1);
        if (space2 == std::string::npos) space2 = request.size();
        path = request.substr(space1 + 1, space2 - space1 - 1);

        // Strip query string
        size_t q = path.find('?');
        if (q != std::string::npos) path = path.substr(0, q);
    }

    static std::string getRequestBody(const std::string& request) {
        size_t body_start = request.find("\r\n\r\n");
        if (body_start == std::string::npos) return "";
        return request.substr(body_start + 4);
    }

    static std::string guessContentType(const std::string& path) {
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".html")
            return "text/html; charset=utf-8";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".css")
            return "text/css";
        if (path.size() >= 3 && path.substr(path.size() - 3) == ".js")
            return "application/javascript";
        if (path.size() >= 5 && path.substr(path.size() - 5) == ".json")
            return "application/json";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".svg")
            return "image/svg+xml";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".png")
            return "image/png";
        if (path.size() >= 4 && path.substr(path.size() - 4) == ".ico")
            return "image/x-icon";
        return "application/octet-stream";
    }

    static void sendAll(http_socket_t sock, const std::string& data) {
        size_t sent = 0;
        while (sent < data.size()) {
            int n = send(sock, data.data() + sent,
                         static_cast<int>(data.size() - sent), 0);
            if (n <= 0) break;
            sent += n;
        }
    }

    int              port_;
    std::string      web_root_;
    compat::Atomic<bool> running_;
    http_socket_t    listen_sock_;
    MetricsCallback  metrics_cb_;

    std::unordered_map<std::string, std::function<std::string(const std::string&)>> endpoints_;
    compat::Mutex    mu_;
    compat::Thread      accept_thread_;
};

}  // namespace network
}  // namespace dcs
