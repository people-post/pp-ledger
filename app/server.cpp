#include "Lib.h"
#include "Server.h"
#include "Logger.h"

#include <iostream>
#include <string>

void printUsage() {
    std::cout << "Usage: pp-ledger-server <port>\n";
    std::cout << "  <port> - Port number to start the server on\n";
}

int main(int argc, char* argv[]) {
    pp::Lib lib;
    auto& rootLogger = pp::logging::getRootLogger();
    rootLogger.info << "PP-Ledger Server v" << lib.getVersion();
    
    if (argc < 2) {
        std::cerr << "Error: Port number required.\n";
        printUsage();
        return 1;
    }
    
    int port = std::stoi(argv[1]);
    
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
