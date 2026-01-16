#include "Client.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Utilities.h"
#include "Logger.h"

#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

void printUsage() {
  std::cout << "Usage: pp-client [OPTIONS] <command> [args...]\n";
  std::cout << "\nOptions:\n";
  std::cout << "  -h <host>        - Server host (optional, default: localhost)\n";
  std::cout << "  -h <host:port>   - Server host and port in one argument\n";
  std::cout << "  -p <port>        - Server port (optional, default: 8517)\n";
  std::cout << "\nCommands:\n";
  std::cout << "  info                           - Get server information\n";
  std::cout << "  wallet <walletId>              - Get wallet balance\n";
  std::cout << "  add-tx <from> <to> <amount>    - Add a transaction\n";
  std::cout << "  validators                     - Get validators list\n";
  std::cout << "  blocks <fromIndex> <count>     - Get blocks\n";
  std::cout << "\nExamples:\n";
  std::cout << "  pp-client info                                    # Use defaults (localhost:8517)\n";
  std::cout << "  pp-client -h localhost info                       # Use default port\n";
  std::cout << "  pp-client -h localhost -p 8080 info               # Specify both\n";
  std::cout << "  pp-client -h localhost:8080 info                  # Use host:port format\n";
  std::cout << "  pp-client -h localhost -p 8080 wallet wallet1\n";
  std::cout << "  pp-client -h localhost:8080 add-tx wallet1 wallet2 100\n";
  std::cout << "  pp-client -p 8080 validators                     # Use default host\n";
  std::cout << "  pp-client -h localhost:8080 blocks 0 10\n";
}

void printInfo(const pp::Client::RespInfo &info) {
  std::cout << "Server Information:\n";
  std::cout << "  Block Count: " << info.blockCount << "\n";
  std::cout << "  Current Slot: " << info.currentSlot << "\n";
  std::cout << "  Current Epoch: " << info.currentEpoch << "\n";
  std::cout << "  Pending Transactions: " << info.pendingTransactions << "\n";
}

void printWalletInfo(const pp::Client::RespWalletInfo &wallet) {
  std::cout << "Wallet: " << wallet.walletId << "\n";
  std::cout << "Balance: " << wallet.balance << "\n";
}

void printValidators(const pp::Client::RespValidators &validators) {
  std::cout << "Validators (" << validators.validators.size() << "):\n";
  if (validators.validators.empty()) {
    std::cout << "  (none)\n";
  } else {
    std::cout << std::left << std::setw(30) << "  ID"
              << "Stake\n";
    std::cout << "  " << std::string(40, '-') << "\n";
    for (const auto &v : validators.validators) {
      std::cout << "  " << std::setw(30) << v.id << v.stake << "\n";
    }
  }
}

void printBlocks(const pp::Client::RespBlocks &blocks) {
  std::cout << "Blocks (" << blocks.blocks.size() << "):\n";
  if (blocks.blocks.empty()) {
    std::cout << "  (none)\n";
  } else {
    for (const auto &block : blocks.blocks) {
      std::cout << "  Block #" << block.index << ":\n";
      std::cout << "    Slot: " << block.slot << "\n";
      std::cout << "    Slot Leader: " << block.slotLeader << "\n";
      std::cout << "    Timestamp: " << block.timestamp << "\n";
      std::cout << "    Hash: " << block.hash << "\n";
      std::cout << "    Previous Hash: " << block.previousHash << "\n";
      std::cout << "    Data Size: " << block.data.size() << " bytes\n";
    }
  }
}

int main(int argc, char *argv[]) {
  auto rootLogger = pp::logging::getRootLogger();
  rootLogger->info << "PP-Ledger Client v" << pp::Client::VERSION;

  if (argc < 2) {
    std::cerr << "Error: Command required.\n";
    printUsage();
    return 1;
  }

  // Parse arguments using simple command-line parser
  std::string host = pp::Client::DEFAULT_HOST;
  uint16_t port = pp::Client::DEFAULT_PORT;
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
        // Format: host:port
        host = parsedHost;
        port = parsedPort;
      } else {
        // Just host (or invalid format, but we'll treat it as host)
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
    } else {
      // Positional argument (command and its arguments)
      positionalArgs.push_back(argv[i]);
    }
  }

  if (positionalArgs.empty()) {
    std::cerr << "Error: Command required.\n";
    printUsage();
    return 1;
  }

  std::string command = positionalArgs[0];

  auto logger = pp::logging::getLogger("client");
  logger->setLevel(
      pp::logging::Level::WARNING); // Reduce verbosity for command-line tool

  pp::Client client;
  if (!client.init(host, static_cast<int>(port))) {
    std::cerr << "Error: Failed to initialize client\n";
    return 1;
  }

  int exitCode = 0;

  if (command == "info") {
      auto result = client.getInfo();
      if (result) {
        printInfo(result.value());
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "wallet") {
      if (positionalArgs.size() < 2) {
        std::cerr << "Error: wallet command requires <walletId>\n";
        printUsage();
        exitCode = 1;
      } else {
        std::string walletId = positionalArgs[1];
        auto result = client.getWalletInfo(walletId);
        if (result) {
          printWalletInfo(result.value());
        } else {
          std::cerr << "Error: " << result.error().message << "\n";
          exitCode = 1;
        }
      }
    } else if (command == "add-tx") {
      if (positionalArgs.size() < 4) {
        std::cerr << "Error: add-tx command requires <fromWallet> <toWallet> "
                     "<amount>\n";
        printUsage();
        exitCode = 1;
      } else {
        std::string fromWallet = positionalArgs[1];
        std::string toWallet = positionalArgs[2];
        int64_t amount = 0;
        if (!pp::utl::parseInt64(positionalArgs[3], amount)) {
          std::cerr << "Error: Invalid amount: " << positionalArgs[3] << "\n";
          exitCode = 1;
        } else {
          // Create and serialize transaction
          pp::Ledger::Transaction tx(fromWallet, toWallet, amount);
          std::string txData = pp::utl::binaryPack(tx);

          auto result = client.addTransaction(txData);
          if (result) {
            std::cout << "Transaction submitted successfully\n";
            std::cout << "  From: " << fromWallet << "\n";
            std::cout << "  To: " << toWallet << "\n";
            std::cout << "  Amount: " << amount << "\n";
          } else {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          }
        }
      }
    } else if (command == "validators") {
      auto result = client.getValidators();
      if (result) {
        printValidators(result.value());
      } else {
        std::cerr << "Error: " << result.error().message << "\n";
        exitCode = 1;
      }
    } else if (command == "blocks") {
      if (positionalArgs.size() < 3) {
        std::cerr << "Error: blocks command requires <fromIndex> <count>\n";
        printUsage();
        exitCode = 1;
      } else {
        uint64_t fromIndex = 0;
        uint64_t count = 0;
        if (!pp::utl::parseUInt64(positionalArgs[1], fromIndex) ||
            !pp::utl::parseUInt64(positionalArgs[2], count)) {
          std::cerr << "Error: Invalid fromIndex or count\n";
          exitCode = 1;
        } else {
          auto result = client.getBlocks(fromIndex, count);
          if (result) {
            printBlocks(result.value());
          } else {
            std::cerr << "Error: " << result.error().message << "\n";
            exitCode = 1;
          }
        }
      }
    } else {
      std::cerr << "Error: Unknown command: " << command << "\n";
      printUsage();
      exitCode = 1;
    }

  client.disconnect();
  return exitCode;
}
