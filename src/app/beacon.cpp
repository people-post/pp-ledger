#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "common/io/Json.h"

#include <cli11.hpp>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
pp::BeaconServer* g_beaconServer = nullptr;

void signalHandler(int signal) {
  if (signal == SIGINT && g_beaconServer) {
    g_beaconServer->setStop(true);
  }
}
} // namespace

int initBeacon(const std::string& workDir, const std::vector<std::string>& genesisKeys) {
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");

  auto result = beaconServer.init(workDir, genesisKeys);
  if (!result) {
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }

  std::cout << "Beacon initialized (genesis signed with " << genesisKeys.size() << " genesis key(s)).\n"
            << "Keep the genesis keys offline: they are the chain's admin keys.\n"
            << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
  return 0;
}

int runBeacon(const std::string& workDir) {
  auto logger = pp::logging::getLogger("pp");
  
  logger.info << "Running beacon with work directory: " << workDir;

  pp::BeaconServer beacon;
  beacon.redirectLogger("pp.B");

  // Set global pointer for signal handler
  g_beaconServer = &beacon;

  auto runResult = beacon.run(workDir);
  
  // Clear global pointer
  g_beaconServer = nullptr;
  
  if (!runResult) {
    logger.error << "Failed to run beacon: " + runResult.error().message;
    std::cerr << "Error: Failed to run beacon: " + runResult.error().message << "\n";
    return 1;
  }

  logger.info << "Beacon stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-beacon - Beacon server for pp-ledger"};
  
  std::string workDir;
  app.add_option("-d,--work-dir", workDir, "Work directory (required)")
      ->required();

  bool initMode = false;
  app.add_flag("--init", initMode, "Initialize a new beacon (genesis) from init-config.json");

  std::vector<std::string> genesisKeys;
  app.add_option("--genesis-key", genesisKeys,
                 "Genesis private key file to sign the genesis block (repeat for M-of-N); --init only");

  bool debugMode = false;
  app.add_flag("--debug", debugMode, "Enable debug logging (default: warning level)");

  app.footer(
    "Initialize (once):\n"
    "  pp-beacon -d <dir> --init --genesis-key genesis.key [--genesis-key ...]\n"
    "  init-config.json sets networkId and the system accounts' public keys\n"
    "  (pp-client keygen -o <name>); a missing file is created as a template.\n"
    "\n"
    "Run:\n"
    "  pp-beacon -d <dir> [--debug]\n"
    "\n"
    "Every field: docs/ops/CONFIGURATION.md"
  );

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel = debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  if (initMode) {
    return initBeacon(workDir, genesisKeys);
  } else {
    return runBeacon(workDir);
  }
}
