#include "FetchClient.h"
#include "FetchServer.h"
#include <iostream>
#include <thread>
#include <chrono>

using namespace pp::network;

int main() {
    std::cout << "Network Example - Using TCP sockets" << std::endl;
    
    // Example 1: Create and start a server
    FetchServer server;
    bool started = server.start(8888, [](const std::string& request) {
        std::cout << "Server received: " << request << std::endl;
        return "Echo: " + request;
    });
    
    if (!started) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    
    std::cout << "Server started on port 8888" << std::endl;
    
    // Give server time to start
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Example 2: Create a client and fetch data
    FetchClient client;
    
    // Async fetch
    std::cout << "Sending async request..." << std::endl;
    client.fetch("127.0.0.1", 8888, "Hello World",
        [](const auto& result) {
            if (result.isOk()) {
                std::cout << "Async response: " << result.value() << std::endl;
            } else {
                std::cerr << "Async error: " << result.error().message << std::endl;
            }
        });
    
    // Wait for async operation
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Synchronous fetch
    std::cout << "Sending sync request..." << std::endl;
    auto result = client.fetchSync("127.0.0.1", 8888, "Hello Sync");
    if (result.isOk()) {
        std::cout << "Sync response: " << result.value() << std::endl;
    } else {
        std::cerr << "Sync error: " << result.error().message << std::endl;
    }
    
    // Stop server
    std::cout << "Stopping server..." << std::endl;
    server.stop();
    
    std::cout << "Example completed successfully!" << std::endl;
    
    return 0;
}
