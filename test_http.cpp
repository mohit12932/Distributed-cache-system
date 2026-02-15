#include "include/network/http_server.h"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    std::cout << "Starting HTTP test server...\n";
    
    dcs::network::HTTPServer server(8080, "web");
    
    server.setMetricsCallback([]() -> std::string {
        return R"({"status":"ok","test":true})";
    });
    
    std::cout << "Calling start()...\n";
    server.start();
    
    std::cout << "Server started. Sleeping...\n";
    std::this_thread::sleep_for(std::chrono::seconds(30));
    
    std::cout << "Stopping...\n";
    server.stop();
    
    std::cout << "Done.\n";
    return 0;
}
