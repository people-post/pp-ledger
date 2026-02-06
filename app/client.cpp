#include "Client.h"
#include "../ledger/Ledger.h"
#include "../consensus/Types.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <ctime>
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
  std::cout << "Current Timestamp: " << formatTimestampLocal(status.currentTimestamp) << "\n";
  std::cout << status.ltsToJson().dump(2) << "\n";
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-client - Command-line client for pp-ledger beacon and miner servers"};
  app.require_subcommand(0, 1);

  // Global options
  bool debug = false;
  app.add_flag("--debug", debug, "Enable debug logging");

  std::string host = pp::Client::DEFAULT_HOST;
  app.add_option("--host", host, "Server host (or host:port)")
      ->capture_default_str();

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Server port (overrides default)")
      ->check(CLI::Range(1, 65535));

  bool connectToBeacon = false;
  app.add_flag("-b,--beacon", connectToBeacon, "Connect to BeaconServer (default port: 8517)");

  bool connectToMiner = false;
  app.add_flag("-m,--miner", connectToMiner, "Connect to MinerServer (default port: 8518)");

  // Local command: keygen
  auto* keygen = app.add_subcommand("keygen", "Generate a new Ed25519 key pair");

  // Beacon commands
  auto* beacon_status = app.add_subcommand("status", "Get beacon/miner status");

  auto* block_cmd = app.add_subcommand("block", "Get block by ID");
  uint64_t blockId = 0;
  block_cmd->add_option("blockId", blockId, "Block ID")->required();

  auto* account_cmd = app.add_subcommand("account", "Get account info by ID");
  uint64_t accountId = 0;
  account_cmd->add_option("accountId", accountId, "Account ID")->required();

  auto* slot_leader_cmd = app.add_subcommand("slot-leader", "Get slot leader for slot");
  uint64_t slot = 0;
  slot_leader_cmd->add_option("slot", slot, "Slot number")->required();

  // Miner commands
  auto* add_tx_cmd = app.add_subcommand("add-tx", "Add a transaction to the miner");
  uint64_t fromWalletId = 0, toWalletId = 0;
  int64_t amount = 0;
  add_tx_cmd->add_option("from", fromWalletId, "From wallet ID")->required();
  add_tx_cmd->add_option("to", toWalletId, "To wallet ID")->required();
  add_tx_cmd->add_option("amount", amount, "Amount to transfer")->required();

  CLI11_PARSE(app, argc, argv);

  // Handle keygen command (no server connection needed)
  if (keygen->parsed()) {
    auto pair = pp::utl::ed25519Generate();
    if (!pair.isOk()) {
      std::cerr << "Error: " << pair.error().message << "\n";
      return 1;
    }
    std::cout << "Ed25519 key pair generated.\n";
    std::cout << "Public key (hex):   " << pp::utl::hexEncode(pair->publicKey) << "\n";
    std::cout << "Private key (hex):  " << pp::utl::hexEncode(pair->privateKey) << "\n";
    std::cout << "\nKeep the private key secret. Use the public key in config (e.g. beacon keys).\n";
    return 0;
  }

  // For server commands, validate beacon/miner flag
  if (!connectToBeacon && !connectToMiner) {
    std::cerr << "Error: Must specify -b/--beacon or -m/--miner for server commands.\n";
    std::cerr << "Run '" << argv[0] << " --help' for more information.\n";
    return 1;
  }

  if (connectToBeacon && connectToMiner) {
    std::cerr << "Error: Cannot connect to both beacon and miner.\n";
    return 1;
  }

  // Parse host:port format if present
  std::string parsedHost = host;
  uint16_t parsedPort = port;
  uint16_t extractedPort = 0;
  if (pp::utl::parseHostPort(host, parsedHost, extractedPort)) {
    if (port == 0) {
      parsedPort = extractedPort;
    }
  }

  // Set default port if not specified
  if (parsedPort == 0) {
    parsedPort = connectToBeacon ? pp::Client::DEFAULT_BEACON_PORT
                                 : pp::Client::DEFAULT_MINER_PORT;
  }

  pp::logging::getRootLogger().setLevel(debug ? pp::logging::Level::DEBUG
                                              : pp::logging::Level::WARNING);
  pp::Client client;

  // Initialize connection
  pp::network::TcpEndpoint endpoint{parsedHost, parsedPort};
  client.setEndpoint(endpoint);

  int exitCode = 0;

  // Handle beacon status command
  if (beacon_status->parsed() && connectToBeacon) {
    auto result = client.fetchBeaconState();
    if (result) {
      printBeaconStatus(result.value());
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle miner status command
  else if (beacon_status->parsed() && connectToMiner) {
    auto result = client.fetchMinerStatus();
    if (result) {
      std::cout << result.value().ltsToJson().dump(2) << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle block command
  else if (block_cmd->parsed()) {
    auto result = client.fetchBlock(blockId);
    if (result) {
      std::cout << result.value().toJson().dump(2) << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle account command
  else if (account_cmd->parsed()) {
    auto result = client.fetchAccountInfo(accountId);
    if (result) {
      std::cout << result.value().toJson().dump(2) << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle slot-leader command (beacon only)
  else if (slot_leader_cmd->parsed()) {
    if (!connectToBeacon) {
      std::cerr << "Error: slot-leader command requires -b/--beacon flag.\n";
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
  // Handle add-tx command (miner only)
  else if (add_tx_cmd->parsed()) {
    if (!connectToMiner) {
      std::cerr << "Error: add-tx command requires -m/--miner flag.\n";
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

  return exitCode;
}
