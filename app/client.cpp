#include "Client.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"
#include "../consensus/Types.hpp"

#include <nlohmann/json.hpp>

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using json = nlohmann::json;

void printUsage() {
  std::cout << "Usage: pp-client [OPTIONS] <command> [args...]\n";
  std::cout << "\nOptions:\n";
  std::cout << "  -h <host>        - Server host (optional, default: localhost)\n";
  std::cout << "  -h <host:port>   - Server host and port in one argument\n";
  std::cout << "  -p <port>        - Server port (optional)\n";
  std::cout << "  -b               - Connect to BeaconServer (default port: 8517)\n";
  std::cout << "  -m               - Connect to MinerServer (default port: 8518)\n";
  std::cout << "\nBeaconServer Commands:\n";
  std::cout << "  block <blockId>                    - Get block by ID\n";
  std::cout << "  current-block                      - Get current block ID\n";
  std::cout << "  stakeholders                       - List stakeholders\n";
  std::cout << "  current-slot                       - Get current slot\n";
  std::cout << "  current-epoch                      - Get current epoch\n";
  std::cout << "  slot-leader <slot>                 - Get slot leader for slot\n";
  std::cout << "\nMinerServer Commands:\n";
  std::cout << "  add-tx <from> <to> <amount>        - Add a transaction\n";
  std::cout << "  pending-txs                        - Get pending transaction count\n";
  std::cout << "  produce-block                      - Produce a new block\n";
  std::cout << "  should-produce                     - Check if should produce block\n";
  std::cout << "  status                             - Get miner status\n";
  std::cout << "\nExamples:\n";
  std::cout << "  pp-client -b current-block                        # Connect to beacon (localhost:8517)\n";
  std::cout << "  pp-client -b -h localhost -p 8517 stakeholders\n";
  std::cout << "  pp-client -m status                               # Connect to miner (localhost:8518)\n";
  std::cout << "  pp-client -m -h localhost:8518 add-tx alice bob 100\n";
}

void printBlockInfo(const pp::Client::BlockInfo &block) {
  std::cout << "Block #" << block.index << ":\n";
  std::cout << "  Slot: " << block.slot << "\n";
  std::cout << "  Slot Leader: " << block.slotLeader << "\n";
  std::cout << "  Timestamp: " << block.timestamp << "\n";
  std::cout << "  Hash: " << block.hash << "\n";
  std::cout << "  Previous Hash: " << block.previousHash << "\n";
  std::cout << "  Data Size: " << block.data.size() << " bytes\n";
}

void printStakeholders(const std::vector<pp::consensus::Stakeholder>& stakeholders) {
  std::cout << "Stakeholders (" << stakeholders.size() << "):\n";
  if (stakeholders.empty()) {
    std::cout << "  (none)\n";
  } else {
    std::cout << std::left << std::setw(30) << "  ID" << "Stake\n";
    std::cout << "  " << std::string(40, '-') << "\n";
    for (const auto &s : stakeholders) {
      std::cout << "  " << std::setw(30) << s.id << s.stake << "\n";
    }
  }
}

void printMinerStatus(const pp::Client::MinerStatus &status) {
  std::cout << "Miner Status:\n";
  std::cout << "  Miner ID: " << status.minerId << "\n";
  std::cout << "  Stake: " << status.stake << "\n";
  std::cout << "  Current Block ID: " << status.currentBlockId << "\n";
  std::cout << "  Current Slot: " << status.currentSlot << "\n";
  std::cout << "  Current Epoch: " << status.currentEpoch << "\n";
  std::cout << "  Pending Transactions: " << status.pendingTransactions << "\n";
  std::cout << "  Is Slot Leader: " << (status.isSlotLeader ? "Yes" : "No") << "\n";
}

int main(int argc, char *argv[]) {
  auto rootLogger = pp::logging::getRootLogger();
  rootLogger.info << "PP-Ledger Client";

  if (argc < 2) {
    std::cerr << "Error: Command required.\n";
    printUsage();
    return 1;
  }

  // Parse arguments
  std::string host = pp::Client::DEFAULT_HOST;
  uint16_t port = 0;
  bool connectToBeacon = false;
  bool connectToMiner = false;
  std::vector<std::string> positionalArgs;

  // Parse options
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "-h") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "Error: -h option requires a host value.\n";
        printUsage();
        return 1;
      }
      std::string hostArg = argv[++i];

      // Check if hostArg contains ':' (host:port format)
      std::string parsedHost;
      uint16_t parsedPort = 0;
      if (pp::utl::parseHostPort(hostArg, parsedHost, parsedPort)) {
        host = parsedHost;
        port = parsedPort;
      } else {
        host = hostArg;
      }
    } else if (strcmp(argv[i], "-p") == 0) {
      if (i + 1 >= argc) {
        std::cerr << "Error: -p option requires a port value.\n";
        printUsage();
        return 1;
      }
      uint16_t parsedPort = 0;
      if (!pp::utl::parsePort(argv[++i], parsedPort)) {
        std::cerr << "Error: Invalid port number: " << argv[i] << "\n";
        printUsage();
        return 1;
      }
      port = parsedPort;
    } else if (strcmp(argv[i], "-b") == 0) {
      connectToBeacon = true;
    } else if (strcmp(argv[i], "-m") == 0) {
      connectToMiner = true;
    } else {
      positionalArgs.push_back(argv[i]);
    }
  }

  if (positionalArgs.empty()) {
    std::cerr << "Error: Command required.\n";
    printUsage();
    return 1;
  }

  // Determine which server to connect to
  if (!connectToBeacon && !connectToMiner) {
    std::cerr << "Error: Must specify -b (beacon) or -m (miner).\n";
    printUsage();
    return 1;
  }

  if (connectToBeacon && connectToMiner) {
    std::cerr << "Error: Cannot connect to both beacon and miner.\n";
    printUsage();
    return 1;
  }

  // Set default port if not specified
  if (port == 0) {
    port = connectToBeacon ? pp::Client::DEFAULT_BEACON_PORT
                           : pp::Client::DEFAULT_MINER_PORT;
  }

  std::string command = positionalArgs[0];

  auto logger = pp::logging::getLogger("client");
  logger.setLevel(pp::logging::Level::WARNING);

  pp::Client client;

  // Initialize connection
  pp::network::TcpEndpoint endpoint{host, port};
  client.setEndpoint(endpoint);

  int exitCode = 0;

  // BeaconServer commands
  if (connectToBeacon) {
    if (command == "block") {
      if (positionalArgs.size() < 2) {
        std::cerr << "Error: block command requires <blockId>\n";
        printUsage();
        exitCode = 1;
      } else {
        uint64_t blockId = 0;
        if (!pp::utl::parseUInt64(positionalArgs[1], blockId)) {
          std::cerr << "Error: Invalid blockId\n";
          exitCode = 1;
        } else {
          auto result = client.fetchBlock(blockId);
          if (result) {
            printBlockInfo(result.value());
          } else {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          }
        }
      }
    } else if (command == "current-block") {
      auto result = client.fetchCurrentBlockId();
      if (result) {
        std::cout << "Current Block ID: " << result.value() << "\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "stakeholders") {
      auto result = client.fetchStakeholders();
      if (result) {
        printStakeholders(result.value());
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "current-slot") {
      auto result = client.fetchCurrentSlot();
      if (result) {
        std::cout << "Current Slot: " << result.value() << "\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "current-epoch") {
      auto result = client.fetchCurrentEpoch();
      if (result) {
        std::cout << "Current Epoch: " << result.value() << "\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "slot-leader") {
      if (positionalArgs.size() < 2) {
        std::cerr << "Error: slot-leader command requires <slot>\n";
        printUsage();
        exitCode = 1;
      } else {
        uint64_t slot = 0;
        if (!pp::utl::parseUInt64(positionalArgs[1], slot)) {
          std::cerr << "Error: Invalid slot\n";
          exitCode = 1;
        } else {
          auto result = client.fetchSlotLeader(slot);
          if (result) {
            std::cout << "Slot Leader for slot " << slot << ": "
                      << result.value() << "\n";
          } else {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          }
        }
      }
    } else {
      std::cerr << "Error: Unknown beacon command: " << command << "\n";
      printUsage();
      exitCode = 1;
    }
  }
  // MinerServer commands
  else {
    if (command == "add-tx") {
      if (positionalArgs.size() < 4) {
        std::cerr << "Error: add-tx command requires <from> <to> <amount>\n";
        printUsage();
        exitCode = 1;
      } else {
        uint64_t fromWalletId = 0;
        uint64_t toWalletId = 0;
        int64_t amount = 0;
        if (!pp::utl::parseUInt64(positionalArgs[1], fromWalletId)) {
          std::cerr << "Error: Invalid from wallet ID: " << positionalArgs[1] << "\n";
          exitCode = 1;
        } else if (!pp::utl::parseUInt64(positionalArgs[2], toWalletId)) {
          std::cerr << "Error: Invalid to wallet ID: " << positionalArgs[2] << "\n";
          exitCode = 1;
        } else if (!pp::utl::parseInt64(positionalArgs[3], amount)) {
          std::cerr << "Error: Invalid amount: " << positionalArgs[3] << "\n";
          exitCode = 1;
        } else {
          // Create transaction JSON
          json txJson = {{"from", fromWalletId},
                         {"to", toWalletId},
                         {"amount", amount}};

          auto result = client.addTransaction(txJson);
          if (result && result.value()) {
            std::cout << "Transaction submitted successfully\n";
            std::cout << "  From: " << fromWalletId << "\n";
            std::cout << "  To: " << toWalletId << "\n";
            std::cout << "  Amount: " << amount << "\n";
          } else {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          }
        }
      }
    } else if (command == "pending-txs") {
      auto result = client.fetchPendingTransactionCount();
      if (result) {
        std::cout << "Pending Transactions: " << result.value() << "\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "produce-block") {
      auto result = client.produceBlock();
      if (result && result.value()) {
        std::cout << "Block production triggered\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "should-produce") {
      auto result = client.fetchIsSlotLeader();
      if (result) {
        std::cout << "Should Produce Block: "
                  << (result.value() ? "Yes" : "No") << "\n";
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "status") {
      auto result = client.fetchMinerStatus();
      if (result) {
        printMinerStatus(result.value());
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else {
      std::cerr << "Error: Unknown miner command: " << command << "\n";
      printUsage();
      exitCode = 1;
    }
  }

  return exitCode;
}
