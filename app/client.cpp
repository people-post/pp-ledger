#include "Client.h"
#include "../ledger/Ledger.h"
#include "../consensus/Types.hpp"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
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

void printBeaconStatus(const pp::Client::BeaconState& status) {
  std::cout << "Current Timestamp: " << formatTimestampLocal(status.currentTimestamp) << "\n";
  std::cout << status.ltsToJson().dump(2) << "\n";
}

static int runAddTx(pp::Client& client, uint64_t fromWalletId, uint64_t toWalletId,
                    int64_t amount, const std::string& key) {
  std::string keyStr = pp::utl::readKey(key);
  if (keyStr.size() >= 2 && keyStr[0] == '0' && (keyStr[1] == 'x' || keyStr[1] == 'X'))
    keyStr = keyStr.substr(2);
  std::string privateKey = pp::utl::hexDecode(keyStr);
  if (privateKey.size() != 32) {
    std::cerr << "Error: --key must be 32 bytes (64 hex chars).\n";
    return 1;
  }
  pp::Ledger::SignedData<pp::Ledger::Transaction> signedTx;
  signedTx.obj.fromWalletId = fromWalletId;
  signedTx.obj.toWalletId = toWalletId;
  signedTx.obj.amount = amount;
  std::string message = pp::utl::binaryPack(signedTx.obj);
  auto sigResult = pp::utl::ed25519Sign(privateKey, message);
  if (!sigResult) {
    std::cerr << "Error: " << sigResult.error().message << "\n";
    return 1;
  }
  signedTx.signatures = {*sigResult};
  auto result = client.addTransaction(signedTx);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction submitted successfully\n";
  return 0;
}

using SignedTx = pp::Ledger::SignedData<pp::Ledger::Transaction>;

static pp::Roe<std::string> readFileContent(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return pp::Error(1, "Cannot open file: " + path);
  std::ostringstream oss;
  oss << f.rdbuf();
  if (!f)
    return pp::Error(2, "Failed to read file: " + path);
  return oss.str();
}

static int runMkTx(uint64_t fromWalletId, uint64_t toWalletId, int64_t amount,
                   const std::string& outputPath) {
  SignedTx signedTx;
  signedTx.obj.fromWalletId = fromWalletId;
  signedTx.obj.toWalletId = toWalletId;
  signedTx.obj.amount = amount;
  signedTx.signatures = {};
  std::string packed = pp::utl::binaryPack(signedTx);
  auto result = pp::utl::writeToNewFile(outputPath, packed);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction written to " << outputPath << "\n";
  return 0;
}

static int runSignTx(const std::string& filePath, const std::string& key) {
  auto content = readFileContent(filePath);
  if (!content) {
    std::cerr << "Error: " << content.error().message << "\n";
    return 1;
  }
  auto signedTxResult = pp::utl::binaryUnpack<SignedTx>(*content);
  if (!signedTxResult) {
    std::cerr << "Error: Invalid signed tx file: " << signedTxResult.error().message << "\n";
    return 1;
  }
  SignedTx signedTx = *signedTxResult;
  std::string keyStr = pp::utl::readKey(key);
  if (keyStr.size() >= 2 && keyStr[0] == '0' && (keyStr[1] == 'x' || keyStr[1] == 'X'))
    keyStr = keyStr.substr(2);
  std::string privateKey = pp::utl::hexDecode(keyStr);
  if (privateKey.size() != 32) {
    std::cerr << "Error: --key must be 32 bytes (64 hex chars).\n";
    return 1;
  }
  std::string message = pp::utl::binaryPack(signedTx.obj);
  auto sigResult = pp::utl::ed25519Sign(privateKey, message);
  if (!sigResult) {
    std::cerr << "Error: " << sigResult.error().message << "\n";
    return 1;
  }
  signedTx.signatures.push_back(*sigResult);
  std::string packed = pp::utl::binaryPack(signedTx);
  std::ofstream f(filePath, std::ios::binary | std::ios::trunc);
  if (!f) {
    std::cerr << "Error: Cannot write file: " << filePath << "\n";
    return 1;
  }
  f << packed;
  if (!f) {
    std::cerr << "Error: Failed to write file: " << filePath << "\n";
    return 1;
  }
  std::cout << "Added signature (" << signedTx.signatures.size() << " total).\n";
  return 0;
}

static int runSubmitTx(pp::Client& client, const std::string& filePath) {
  auto content = readFileContent(filePath);
  if (!content) {
    std::cerr << "Error: " << content.error().message << "\n";
    return 1;
  }
  auto signedTxResult = pp::utl::binaryUnpack<SignedTx>(*content);
  if (!signedTxResult) {
    std::cerr << "Error: Invalid signed tx file: " << signedTxResult.error().message << "\n";
    return 1;
  }
  auto result = client.addTransaction(*signedTxResult);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction submitted successfully\n";
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-client - Command-line client for pp-ledger beacon and miner servers"};
  app.require_subcommand(1);

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

  // Miner commands
  auto* add_tx_cmd = app.add_subcommand("add-tx", "Add a transaction to the miner");
  uint64_t fromWalletId = 0, toWalletId = 0;
  int64_t amount = 0;
  std::string key;
  add_tx_cmd->add_option("from", fromWalletId, "From wallet ID")->required();
  add_tx_cmd->add_option("to", toWalletId, "To wallet ID")->required();
  add_tx_cmd->add_option("amount", amount, "Amount to transfer")->required();
  add_tx_cmd->add_option("-k,--key", key, "Private key (hex or file) to sign the transaction")
      ->required();

  // mk-tx: create unsigned SignedData and save to file
  auto* mk_tx_cmd = app.add_subcommand("mk-tx", "Create unsigned transaction file");
  uint64_t mk_from = 0, mk_to = 0;
  int64_t mk_amount = 0;
  std::string mk_output;
  mk_tx_cmd->add_option("from", mk_from, "From wallet ID")->required();
  mk_tx_cmd->add_option("to", mk_to, "To wallet ID")->required();
  mk_tx_cmd->add_option("amount", mk_amount, "Amount to transfer")->required();
  mk_tx_cmd->add_option("-o,--output", mk_output, "Output file (must not exist)")
      ->required();

  // sign-tx: add a signature to an existing tx file
  auto* sign_tx_cmd = app.add_subcommand("sign-tx", "Add signature to a transaction file");
  std::string sign_tx_file;
  std::string sign_key;
  sign_tx_cmd->add_option("file", sign_tx_file, "Transaction file to sign")->required();
  sign_tx_cmd->add_option("-k,--key", sign_key, "Private key (hex or file) to sign")
      ->required();

  // submit-tx: submit signed tx file to miner
  auto* submit_tx_cmd = app.add_subcommand("submit-tx", "Submit signed transaction file to miner");
  std::string submit_tx_file;
  submit_tx_cmd->add_option("file", submit_tx_file, "Signed transaction file")->required();

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

  // Handle mk-tx (no server connection needed)
  if (mk_tx_cmd->parsed()) {
    return runMkTx(mk_from, mk_to, mk_amount, mk_output);
  }

  // Handle sign-tx (no server connection needed)
  if (sign_tx_cmd->parsed()) {
    return runSignTx(sign_tx_file, sign_key);
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
    auto result = client.fetchUserAccount(accountId);
    if (result) {
      std::cout << result.value().toJson().dump(2) << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle add-tx command (miner only)
  else if (add_tx_cmd->parsed()) {
    if (!connectToMiner) {
      std::cerr << "Error: add-tx command requires -m/--miner flag.\n";
      exitCode = 1;
    } else {
      exitCode = runAddTx(client, fromWalletId, toWalletId, amount, key);
    }
  }
  // Handle submit-tx command (miner only)
  else if (submit_tx_cmd->parsed()) {
    if (!connectToMiner) {
      std::cerr << "Error: submit-tx command requires -m/--miner flag.\n";
      exitCode = 1;
    } else {
      exitCode = runSubmitTx(client, submit_tx_file);
    }
  }

  return exitCode;
}
