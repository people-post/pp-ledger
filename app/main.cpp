#include <iostream>
#include <string>
#include "Lib.h"
#include "Client.h"
#include "Server.h"

void printUsage() {
    std::cout << "Usage: pp-ledger [server|client] [options]\n";
    std::cout << "  server <port>       - Start server on specified port\n";
    std::cout << "  client <host> <port> - Connect client to host:port\n";
}

int runServer(int port) {
    std::cout << "Starting server on port " << port << "...\n";
    
    pp::Server server;
    if (server.start(port)) {
        std::cout << "Server started successfully.\n";
        std::cout << "Press Enter to stop the server...\n";
        std::cin.get();
        server.stop();
        std::cout << "Server stopped.\n";
        return 0;
    } else {
        std::cerr << "Failed to start server.\n";
        return 1;
    }
}

int runClient(const std::string& host, int port) {
    std::cout << "Connecting to " << host << ":" << port << "...\n";
    
    pp::Client client;
    if (client.connect(host, port)) {
        std::cout << "Connected successfully.\n";
        std::cout << "Press Enter to disconnect...\n";
        std::cin.get();
        client.disconnect();
        std::cout << "Disconnected.\n";
        return 0;
    } else {
        std::cerr << "Failed to connect.\n";
        return 1;
    }
}

int main(int argc, char* argv[]) {
    pp::Lib lib;
    std::cout << "PP-Ledger v" << lib.getVersion() << "\n\n";
    
    if (argc < 2) {
        printUsage();
        return 1;
    }
    
    std::string mode = argv[1];
    
    if (mode == "server") {
        if (argc < 3) {
            std::cerr << "Error: Port number required for server mode.\n";
            printUsage();
            return 1;
        }
        int port = std::stoi(argv[2]);
        return runServer(port);
    } 
    else if (mode == "client") {
        if (argc < 4) {
            std::cerr << "Error: Host and port required for client mode.\n";
            printUsage();
            return 1;
        }
        std::string host = argv[2];
        int port = std::stoi(argv[3]);
        return runClient(host, port);
    } 
    else {
        std::cerr << "Error: Unknown mode '" << mode << "'.\n";
        printUsage();
        return 1;
    }
    
    return 0;
}
