#include "Client.h"
#include "../ledger/Ledger.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Utilities.h"
#include "Logger.h"

#include <iomanip>
#include <iostream>
#include <string>

void printUsage() {
  std::cout << "Usage: pp-ledger-client <host> <port> <command> [args...]\n";
  std::cout << "\nCommands:\n";
  std::cout << "  info                           - Get server information\n";
  std::cout << "  wallet <walletId>              - Get wallet balance\n";
  std::cout << "  add-tx <from> <to> <amount>    - Add a transaction\n";
  std::cout << "  validators                     - Get validators list\n";
  std::cout << "  blocks <fromIndex> <count>     - Get blocks\n";
  std::cout << "\nExamples:\n";
  std::cout << "  pp-ledger-client localhost 8080 info\n";
  std::cout << "  pp-ledger-client localhost 8080 wallet wallet1\n";
  std::cout << "  pp-ledger-client localhost 8080 add-tx wallet1 wallet2 100\n";
  std::cout << "  pp-ledger-client localhost 8080 validators\n";
  std::cout << "  pp-ledger-client localhost 8080 blocks 0 10\n";
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

  if (argc < 4) {
    std::cerr << "Error: Host, port, and command required.\n";
    printUsage();
    return 1;
  }

  std::string host = argv[1];
  uint16_t port = 0;
  if (!pp::utl::parsePort(argv[2], port)) {
    std::cerr << "Error: Invalid port number: " << argv[2] << "\n";
    return 1;
  }
  std::string command = argv[3];

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
      if (argc < 5) {
        std::cerr << "Error: wallet command requires <walletId>\n";
        printUsage();
        exitCode = 1;
      } else {
        std::string walletId = argv[4];
        auto result = client.getWalletInfo(walletId);
        if (result) {
          printWalletInfo(result.value());
        } else {
          std::cerr << "Error: " << result.error().message << "\n";
          exitCode = 1;
        }
      }
    } else if (command == "add-tx") {
      if (argc < 7) {
        std::cerr << "Error: add-tx command requires <fromWallet> <toWallet> "
                     "<amount>\n";
        printUsage();
        exitCode = 1;
      } else {
        std::string fromWallet = argv[4];
        std::string toWallet = argv[5];
        int64_t amount = 0;
        if (!pp::utl::parseInt64(argv[6], amount)) {
          std::cerr << "Error: Invalid amount: " << argv[6] << "\n";
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
      if (argc < 6) {
        std::cerr << "Error: blocks command requires <fromIndex> <count>\n";
        printUsage();
        exitCode = 1;
      } else {
        uint64_t fromIndex = 0;
        uint64_t count = 0;
        if (!pp::utl::parseUInt64(argv[4], fromIndex) ||
            !pp::utl::parseUInt64(argv[5], count)) {
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
