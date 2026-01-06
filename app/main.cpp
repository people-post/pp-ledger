#include "Lib.h"
#include "Client.h"
#include "Server.h"
#include "Logger.h"

#include <iostream>
#include <string>

void printUsage() {
    std::cout << "Usage: pp-ledger [server|client] [options]\n";
    std::cout << "  server <port>       - Start server on specified port\n";
    std::cout << "  client <host> <port> - Connect client to host:port\n";
}

int runServer(int port) {
    auto& logger = pp::logging::getLogger("server");
    logger.setLevel(pp::logging::Level::INFO);
    logger.addFileHandler("server.log", pp::logging::Level::DEBUG);
    
    logger.info << "Starting server on port " << port;
    
    pp::Server server;
    if (server.start(port)) {
        logger.info << "Server started successfully";
        std::cout << "Press Enter to stop the server...\n";
        std::cin.get();
        server.stop();
        logger.info << "Server stopped";
        return 0;
    } else {
        logger.error << "Failed to start server";
        return 1;
    }
}

int runClient(const std::string& host, int port) {
    auto& logger = pp::logging::getLogger("client");
    logger.setLevel(pp::logging::Level::INFO);
    logger.addFileHandler("client.log", pp::logging::Level::DEBUG);
    
    logger.info << "Connecting to " << host << ":" << port;
    
    pp::Client client;
    if (client.connect(host, port)) {
        logger.info << "Connected successfully";
        std::cout << "Press Enter to disconnect...\n";
        std::cin.get();
        client.disconnect();
        logger.info << "Disconnected";
        return 0;
    } else {
        logger.error << "Failed to connect";
        return 1;
    }
}

int main(int argc, char* argv[]) {
    pp::Lib lib;
    auto& rootLogger = pp::logging::getRootLogger();
    rootLogger.info << "PP-Ledger v" << lib.getVersion();
    
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
