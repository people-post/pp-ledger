#include "Module.h"
#include "Server.h"
#include "Client.h"
#include "BlockChain.h"
#include "BlockDir.h"
#include "BlockFile.h"
#include "Ledger.h"

#include <iostream>

int main() {
    std::cout << "=== Testing Module Base Class ===\n\n";
    
    // Create instances of all Module-based classes
    std::cout << "1. Creating Module instances:\n";
    pp::Server server;
    pp::Client client;
    pp::BlockChain blockchain(2);
    pp::Ledger ledger(2);
    
    std::cout << "  - Server logger name: " << server.getLoggerName() << "\n";
    std::cout << "  - Client logger name: " << client.getLoggerName() << "\n";
    std::cout << "  - BlockChain logger name: " << blockchain.getLoggerName() << "\n";
    std::cout << "  - Ledger logger name: " << ledger.getLoggerName() << "\n";
    
    // Test logging through Module base class
    std::cout << "\n2. Testing logging through Module classes:\n";
    server.log().info << "Message from Server module";
    client.log().info << "Message from Client module";
    blockchain.log().info << "Message from BlockChain module";
    ledger.log().info << "Message from Ledger module";
    
    // Test logger redirection
    std::cout << "\n3. Testing logger redirection:\n";
    std::cout << "Before redirect:\n";
    server.log().info << "Server message before redirect";
    
    std::cout << "\nRedirecting 'server' logger to 'main':\n";
    server.redirectLogger("main");
    
    server.log().info << "Server message after redirect (shows as main)";
    
    std::cout << "\nClearing redirect:\n";
    server.clearLoggerRedirect();
    server.log().info << "Server message after clearing redirect";
    
    // Test BlockFile and BlockDir
    std::cout << "\n4. Testing BlockFile and BlockDir modules:\n";
    pp::BlockFile blockFile;
    pp::BlockDir blockDir;
    
    std::cout << "  - BlockFile logger name: " << blockFile.getLoggerName() << "\n";
    std::cout << "  - BlockDir logger name: " << blockDir.getLoggerName() << "\n";
    
    blockFile.log().info << "Message from BlockFile module";
    blockDir.log().info << "Message from BlockDir module";
    
    // Test multiple modules with different redirects
    std::cout << "\n5. Testing independent redirects for different modules:\n";
    client.redirectLogger("system");
    blockchain.redirectLogger("system");
    
    std::cout << "Client redirected to 'system':\n";
    client.log().info << "Client message going to system logger";
    
    std::cout << "BlockChain redirected to 'system':\n";
    blockchain.log().info << "BlockChain message going to system logger";
    
    std::cout << "\n=== Test Complete ===\n";
    
    return 0;
}
