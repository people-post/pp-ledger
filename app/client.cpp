#include "Client.h"
#include "../ledger/Ledger.h"
#include "../consensus/Types.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <nlohmann/json.hpp>

#include <ctime>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

static std::string formatTimestampLocal(int64_t unixSeconds) {
  time_t t = static_cast<time_t>(unixSeconds);
  std::tm* local = std::localtime(&t);
  if (!local) return std::to_string(unixSeconds);
  char buf[64];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", local) == 0)
    return std::to_string(unixSeconds);
  return std::string(buf);
}

using json = nlohmann::json;

void printUsage() {
  std::cout << "Usage: pp-client [OPTIONS] <command> [args...]\n";
  std::cout << "\nOptions:\n";
  std::cout << "  --debug          - Enable debug logging (default: false)\n";
  std::cout << "  -h <host>        - Server host (optional, default: localhost)\n";
  std::cout << "  -h <host:port>   - Server host and port in one argument\n";
  std::cout << "  -p <port>        - Server port (optional)\n";
  std::cout << "  -b               - Connect to BeaconServer (default port: 8517)\n";
  std::cout << "  -m               - Connect to MinerServer (default port: 8518)\n";
  std::cout << "\nBeaconServer Commands:\n";
  std::cout << "  block <blockId>                    - Get block by ID\n";
  std::cout << "  status                             - Get beacon status\n";
  std::cout << "  slot-leader <slot>                 - Get slot leader for slot\n";
  std::cout << "\nMinerServer Commands:\n";
  std::cout << "  add-tx <from> <to> <amount>        - Add a transaction\n";
  std::cout << "  status                             - Get miner status\n";
  std::cout << "\nExamples:\n";
  std::cout << "  pp-client -b status                               # Connect to beacon (localhost:8517)\n";
  std::cout << "  pp-client -b -h localhost -p 8517 block 0\n";
  std::cout << "  pp-client -m status                               # Connect to miner (localhost:8518)\n";
  std::cout << "  pp-client -m -h localhost:8518 add-tx 1 2 100\n";
}

void printBlockInfo(const pp::Ledger::ChainNode& node) {
  const auto& block = node.block;
  std::cout << "Block #" << block.index << ":\n";
  std::cout << "  Slot: " << block.slot << "\n";
  std::cout << "  Slot Leader: " << block.slotLeader << "\n";
  std::cout << "  Timestamp: " << formatTimestampLocal(block.timestamp) << "\n";
  std::cout << "  Hash: " << node.hash << "\n";
  std::cout << "  Previous Hash: " << block.previousHash << "\n";
  std::cout << "  Transactions: " << block.signedTxes.size() << "\n";
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

void printBeaconStatus(const pp::Client::BeaconState& status) {
  std::cout << "Beacon Status:\n";
  std::cout << "  Last Checkpoint ID: " << status.lastCheckpointId << "\n";
  std::cout << "  Checkpoint ID: " << status.checkpointId << "\n";
  std::cout << "  Next Block ID: " << status.nextBlockId << "\n";
  std::cout << "  Current Slot: " << status.currentSlot << "\n";
  std::cout << "  Current Epoch: " << status.currentEpoch << "\n";
  std::cout << "  Current Timestamp: " << formatTimestampLocal(status.currentTimestamp) << "\n";
}

void printMinerStatus(const pp::Client::MinerStatus &status) {
  std::cout << "Miner Status:\n";
  std::cout << "  Miner ID: " << status.minerId << "\n";
  std::cout << "  Stake: " << status.stake << "\n";
  std::cout << "  Next Block ID: " << status.nextBlockId << "\n";
  std::cout << "  Current Slot: " << status.currentSlot << "\n";
  std::cout << "  Current Epoch: " << status.currentEpoch << "\n";
  std::cout << "  Pending Transactions: " << status.pendingTransactions << "\n";
  std::cout << "  Is Slot Leader: " << (status.isSlotLeader ? "Yes" : "No") << "\n";
}

int main(int argc, char *argv[]) {
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
  bool debug = false;
  std::vector<std::string> positionalArgs;

  // Parse options
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--debug") == 0) {
      debug = true;
    } else if (strcmp(argv[i], "-h") == 0) {
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

  pp::logging::getRootLogger().setLevel(debug ? pp::logging::Level::DEBUG
                                              : pp::logging::Level::WARNING);
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
    } else if (command == "status") {
      auto result = client.fetchBeaconState();
      if (result) {
        printBeaconStatus(result.value());
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
          pp::Ledger::SignedData<pp::Ledger::Transaction> signedTx;
          signedTx.obj.fromWalletId = fromWalletId;
          signedTx.obj.toWalletId = toWalletId;
          signedTx.obj.amount = amount;
          signedTx.signatures = {};

          auto result = client.addTransaction(signedTx);
          if (!result) {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          } else {
            std::cout << "Transaction submitted successfully\n";
          }
        }
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
