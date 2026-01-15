#include "Lib.h"
#include "Client.h"
#include "Logger.h"

#include <iostream>
#include <string>

void printUsage() {
    std::cout << "Usage: pp-ledger-client <host> <port>\n";
    std::cout << "  <host> - Server hostname or IP address\n";
    std::cout << "  <port> - Server port number\n";
}

int main(int argc, char* argv[]) {
    pp::Lib lib;
    auto& rootLogger = pp::logging::getRootLogger();
    rootLogger.info << "PP-Ledger Client v" << lib.getVersion();
    
    if (argc < 3) {
        std::cerr << "Error: Host and port required.\n";
        printUsage();
        return 1;
    }
    
    std::string host = argv[1];
    int port = std::stoi(argv[2]);
    
    auto& logger = pp::logging::getLogger("client");
    logger.setLevel(pp::logging::Level::INFO);
    logger.addFileHandler("client.log", pp::logging::Level::DEBUG);
    
    logger.info << "Connecting to " << host << ":" << port;
    
    pp::Client client;
    if (client.init(host, port)) {
        logger.info << "Connected successfully";
        
        // Example: Query wallet info
        auto walletResult = client.getWalletInfo("test_wallet");
        if (walletResult) {
            logger.info << "Wallet balance: " << walletResult.value().balance;
        } else {
            logger.warning << "Failed to get wallet info: " << walletResult.error().message;
        }
        
        std::cout << "Press Enter to disconnect...\n";
        std::cin.get();
        client.disconnect();
        logger.info << "Disconnected";
        return 0;
    } else {
        logger.error << "Failed to initialize client";
        return 1;
    }
}
