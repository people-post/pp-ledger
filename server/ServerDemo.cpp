#include <iostream>
#include "Server.h"

using namespace pp;

int main() {
    std::cout << "Creating Server with blockchain difficulty of 2..." << std::endl;
    Server server(2);
    
    std::cout << "Starting server on port 8080..." << std::endl;
    if (!server.start(8080)) {
        std::cerr << "Failed to start server" << std::endl;
        return 1;
    }
    
    std::cout << "Registering stakeholders..." << std::endl;
    server.registerStakeholder("alice", 1000);
    server.registerStakeholder("bob", 2000);
    server.registerStakeholder("charlie", 500);
    
    std::cout << "Setting slot duration to 2 seconds..." << std::endl;
    server.setSlotDuration(2);
    
    std::cout << "Submitting transactions..." << std::endl;
    server.submitTransaction("Transfer 100 from alice to bob");
    server.submitTransaction("Transfer 50 from bob to charlie");
    
    std::cout << "Current state:" << std::endl;
    std::cout << "  - Pending transactions: " << server.getPendingTransactionCount() << std::endl;
    std::cout << "  - Current slot: " << server.getCurrentSlot() << std::endl;
    std::cout << "  - Current epoch: " << server.getCurrentEpoch() << std::endl;
    std::cout << "  - Block count: " << server.getBlockCount() << std::endl;
    
    // Access the consensus and ledger directly
    std::cout << "Consensus info:" << std::endl;
    std::cout << "  - Total stakeholders: " << server.getConsensus().getStakeholderCount() << std::endl;
    std::cout << "  - Total stake: " << server.getConsensus().getTotalStake() << std::endl;
    
    std::cout << "Ledger info:" << std::endl;
    std::cout << "  - Blockchain is valid: " << (server.getLedger().getBlockChain().isValid() ? "yes" : "no") << std::endl;
    
    std::cout << "Stopping server..." << std::endl;
    server.stop();
    
    std::cout << "Server test completed successfully!" << std::endl;
    return 0;
}
