/**
 * Multi-Node Server Example
 * 
 * Demonstrates how to use the Server class with P2P networking enabled
 * to create a multi-node blockchain network.
 */

#include "Server.h"
#include "Logger.h"
#include <iostream>
#include <thread>
#include <chrono>

void runNode(const std::string& nodeId, int port, uint16_t p2pPort, const std::vector<std::string>& bootstrapPeers) {
    auto& logger = logging::getLogger("node-" + nodeId);
    logger.info << "Starting node: " << nodeId;
    
    // Create server instance
    pp::Server server(2);  // Difficulty 2
    
    // Configure node as a stakeholder
    server.registerStakeholder(nodeId, 1000);  // 1000 stake
    server.setSlotDuration(5);  // 5 second slots
    
    // Configure P2P network
    pp::Server::NetworkConfig networkConfig;
    networkConfig.enableP2P = true;
    networkConfig.nodeId = nodeId;
    networkConfig.bootstrapPeers = bootstrapPeers;
    networkConfig.listenAddr = "0.0.0.0";
    networkConfig.p2pPort = p2pPort;
    networkConfig.maxPeers = 50;
    
    // Start server with P2P enabled
    if (!server.start(port, networkConfig)) {
        logger.error << "Failed to start server";
        return;
    }
    
    logger.info << "Node started successfully";
    logger.info << "P2P enabled: " << (server.isP2PEnabled() ? "yes" : "no");
    
    // Submit some transactions
    for (int i = 0; i < 5; i++) {
        std::string tx = nodeId + "-tx-" + std::to_string(i);
        server.submitTransaction(tx);
        logger.info << "Submitted transaction: " << tx;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    
    // Run for a while
    std::this_thread::sleep_for(std::chrono::seconds(30));
    
    // Print status
    logger.info << "Connected peers: " << server.getPeerCount();
    logger.info << "Block count: " << server.getBlockCount();
    logger.info << "Current epoch: " << server.getCurrentEpoch();
    logger.info << "Current slot: " << server.getCurrentSlot();
    
    // Stop server
    server.stop();
    logger.info << "Node stopped";
}

int main() {
    // Initialize logging
    logging::getLogger("main").info << "Multi-Node Blockchain Demo";
    logging::getLogger("main").info << "P2P support: ENABLED";
    
    // Example 1: Single node (no P2P peers)
    {
        logging::getLogger("main").info << "\n=== Example 1: Single Node ===";
        
        pp::Server server(2);
        server.registerStakeholder("node-single", 1000);
        server.setSlotDuration(5);
        
        // Start without P2P
        server.start(8080);
        
        // Submit transactions
        for (int i = 0; i < 3; i++) {
            server.submitTransaction("tx-" + std::to_string(i));
        }
        
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        std::cout << "Blocks produced: " << server.getBlockCount() << std::endl;
        
        server.stop();
    }
    
    // Example 2: Multi-node network (3 nodes)
    {
        logging::getLogger("main").info << "\n=== Example 2: Multi-Node Network ===";
        
        // In a real deployment, these would be actual network addresses
        // For this example, we'd need to run each node in a separate process
        
        std::vector<std::string> bootstrapPeers = {
            "127.0.0.1:9001",
            "127.0.0.1:9002"
        };
        
        // Note: To actually test multi-node, you would run each node in a separate process:
        // ./node1 --node-id node1 --port 8081 --p2p-port 9001
        // ./node2 --node-id node2 --port 8082 --p2p-port 9002 --bootstrap 127.0.0.1:9001
        // ./node3 --node-id node3 --port 8083 --p2p-port 9003 --bootstrap 127.0.0.1:9001
        
        std::cout << "\nTo run multi-node network:" << std::endl;
        std::cout << "1. Start bootstrap node:" << std::endl;
        std::cout << "   Node 1 (bootstrap) on port 8081, P2P port 9001" << std::endl;
        std::cout << "\n2. Start additional nodes:" << std::endl;
        std::cout << "   Node 2 on port 8082, P2P port 9002, connecting to 127.0.0.1:9001" << std::endl;
        std::cout << "   Node 3 on port 8083, P2P port 9003, connecting to 127.0.0.1:9001" << std::endl;
        std::cout << "\n3. Nodes will:" << std::endl;
        std::cout << "   - Discover each other through bootstrap node" << std::endl;
        std::cout << "   - Participate in Ouroboros consensus" << std::endl;
        std::cout << "   - Only slot leader produces blocks" << std::endl;
        std::cout << "   - Broadcast new blocks to all peers" << std::endl;
        std::cout << "   - Sync blockchain state from peers" << std::endl;
    }
    
    // Example 3: Server with P2P enabled
    {
        logging::getLogger("main").info << "\n=== Example 3: P2P Enabled Server ===";
        
        pp::Server server(2);
        server.registerStakeholder("demo-node", 1000);
        server.setSlotDuration(5);
        
        pp::Server::NetworkConfig config;
        config.enableP2P = true;
        config.nodeId = "demo-node";
        config.listenAddr = "0.0.0.0";
        config.p2pPort = 9000;
        // No bootstrap peers - this is a standalone node
        
        if (server.start(8080, config)) {
            std::cout << "Server started with P2P enabled" << std::endl;
            std::cout << "P2P enabled: " << server.isP2PEnabled() << std::endl;
            std::cout << "Connected peers: " << server.getPeerCount() << std::endl;
            
            // Connect to a peer manually
            // server.connectToPeer("127.0.0.1:9001");
            
            // Submit transactions
            for (int i = 0; i < 3; i++) {
                server.submitTransaction("demo-tx-" + std::to_string(i));
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
            
            std::this_thread::sleep_for(std::chrono::seconds(10));
            
            std::cout << "\nFinal Status:" << std::endl;
            std::cout << "  Peers: " << server.getPeerCount() << std::endl;
            std::cout << "  Blocks: " << server.getBlockCount() << std::endl;
            std::cout << "  Epoch: " << server.getCurrentEpoch() << std::endl;
            std::cout << "  Slot: " << server.getCurrentSlot() << std::endl;
            
            server.stop();
        }
    }
    
    return 0;
}
