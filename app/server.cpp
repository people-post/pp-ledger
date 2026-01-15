#include "Lib.h"
#include "Server.h"
#include "Logger.h"

#include <iostream>
#include <string>
#include <cstring>

void printUsage() {
    std::cout << "Usage: pp-ledger-server -d <data-dir> <port>\n";
    std::cout << "  -d <data-dir>  - Data directory (required)\n";
    std::cout << "  <port>         - Port number to start the server on\n";
    std::cout << "\n";
    std::cout << "The data directory will contain:\n";
    std::cout << "  - active/  - Active (hot) blocks\n";
    std::cout << "  - archive/ - Archived (cold) blocks\n";
    std::cout << "\n";
    std::cout << "Example:\n";
    std::cout << "  pp-ledger-server -d ./data 8080\n";
}

int main(int argc, char* argv[]) {
    pp::Lib lib;
    auto& rootLogger = pp::logging::getRootLogger();
    rootLogger.info << "PP-Ledger Server v" << lib.getVersion();
    
    if (argc < 4) {
        std::cerr << "Error: Data directory and port required.\n";
        printUsage();
        return 1;
    }
    
    std::string dataDir;
    int port = 0;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0) {
            if (i + 1 < argc) {
                dataDir = argv[++i];
            } else {
                std::cerr << "Error: -d option requires a directory path.\n";
                printUsage();
                return 1;
            }
        } else {
            // Assume it's the port
            try {
                port = std::stoi(argv[i]);
            } catch (const std::exception& e) {
                std::cerr << "Error: Invalid port number: " << argv[i] << "\n";
                printUsage();
                return 1;
            }
        }
    }
    
    if (dataDir.empty()) {
        std::cerr << "Error: Data directory (-d) is required.\n";
        printUsage();
        return 1;
    }
    
    if (port == 0) {
        std::cerr << "Error: Port number is required.\n";
        printUsage();
        return 1;
    }
    
    auto& logger = pp::logging::getLogger("server");
    logger.setLevel(pp::logging::Level::INFO);
    logger.addFileHandler("server.log", pp::logging::Level::DEBUG);
    
    logger.info << "Starting server on port " << port << " with data directory: " << dataDir;
    
    pp::Server server;
    if (server.start(port, dataDir)) {
        logger.info << "Server started successfully";
        std::cout << "Server running on port " << port << "\n";
        std::cout << "Data directory: " << dataDir << "\n";
        std::cout << "Press Enter to stop the server...\n";
        std::cin.get();
        server.stop();
        logger.info << "Server stopped";
        return 0;
    } else {
        logger.error << "Failed to start server";
        std::cerr << "Error: Failed to start server\n";
        return 1;
    }
}
