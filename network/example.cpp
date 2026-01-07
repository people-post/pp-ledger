#include "FetchClient.h"
#include "FetchServer.h"
#include <libp2p/injector/host_injector.hpp>
#include <iostream>

using namespace pp::network;

int main() {
    // Create libp2p injector and host
    auto injector = libp2p::injector::makeHostInjector();
    auto host = injector.create<std::shared_ptr<libp2p::Host>>();
    
    // Start the host
    host->start();
    
    // Example 1: Create and start a server
    FetchServer server(host);
    server.start("/myapp/fetch/1.0.0", [](const std::string& request) {
        std::cout << "Server received: " << request << std::endl;
        return "Echo: " + request;
    });
    
    std::cout << "Server started on protocol /myapp/fetch/1.0.0" << std::endl;
    
    // Example 2: Create a client and fetch data
    FetchClient client(host);
    
    // Get server's peer info (in real application, you'd discover this)
    auto serverPeerInfo = host->getPeerInfo();
    
    // Async fetch
    client.fetch(serverPeerInfo, "/myapp/fetch/1.0.0", "Hello World",
        [](const auto& result) {
            if (result.isOk()) {
                std::cout << "Async response: " << result.value() << std::endl;
            } else {
                std::cerr << "Async error: " << result.error().message << std::endl;
            }
        });
    
    // Synchronous fetch
    auto result = client.fetchSync(serverPeerInfo, "/myapp/fetch/1.0.0", "Hello Sync");
    if (result.isOk()) {
        std::cout << "Sync response: " << result.value() << std::endl;
    } else {
        std::cerr << "Sync error: " << result.error().message << std::endl;
    }
    
    // Wait for async operations
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Stop server
    server.stop();
    
    // Stop host
    host->stop();
    
    return 0;
}
