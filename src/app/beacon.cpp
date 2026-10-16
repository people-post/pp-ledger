#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "common/io/Json.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {
pp::BeaconServer* g_beaconServer = nullptr;

/** Set by a signal: no restart follows the current run (beacon-authority bp1). */
std::atomic<bool> g_stopRequested{false};

void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_stopRequested = true;
    if (g_beaconServer) {
      g_beaconServer->setStop(true);
    }
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

int runBeacon(const std::string& workDir, uint64_t approvedRewindGeneration, bool promote) {
  auto logger = pp::logging::getLogger("pp");

  logger.info << "Running beacon with work directory: " << workDir;

  // A rewind is applied by a restart (beacon-authority bp1, B022): run a fresh server on the same
  // work directory for as long as the last one asks to be restarted.
  while (!g_stopRequested) {
    pp::BeaconServer beacon;
    beacon.redirectLogger("pp.B");
    beacon.setApprovedRewindGeneration(approvedRewindGeneration);
    beacon.setPromote(promote);
    promote = false; // the first start promotes; a restart (a rewind) runs the beacon it became
    g_beaconServer = &beacon; // for the signal handler

    auto runResult = beacon.run(workDir);
    g_beaconServer = nullptr;

    if (!runResult) {
      logger.error << "Failed to run beacon: " + runResult.error().message;
      std::cerr << "Error: Failed to run beacon: " + runResult.error().message << "\n";
      return 1;
    }
    if (!beacon.restartRequested()) {
      break;
    }
    logger.warning << "Beacon restarting";
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

  bool promote = false;
  app.add_flag("--promote", promote,
               "Turn this relay's work directory into the beacon taking over (beacon-authority case 2): the "
               "admins' certificate held here must name keys/beacon-signing.pub and carry a cutover");

  uint64_t approvedRewindGeneration = 0;
  app.add_option("--approve-rewind", approvedRewindGeneration,
                 "Apply the rewind ordered by this beacon generation although it reaches back further "
                 "than a node rewinds on its own (7 days of slots); check the certificate first");

  app.footer(
    "Initialize (once):\n"
    "  pp-beacon -d <dir> --init --genesis-key genesis.key [--genesis-key ...]\n"
    "  init-config.json sets networkId and the system accounts' public keys\n"
    "  (pp-client keygen -o <name>); a missing file is created as a template.\n"
    "\n"
    "Run:\n"
    "  pp-beacon -d <dir> [--debug]\n"
    "\n"
    "Take over (a relay's directory becomes the new beacon; once):\n"
    "  pp-beacon -d <relay dir> --promote\n"
    "\n"
    "Every field: docs/ops/CONFIGURATION.md"
  );

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel = debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Ctrl+C, and the SIGTERM a service manager (docker, systemd) stops with: stop cleanly, so the
  // ledger commits what it appended.
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  if (initMode && promote) {
    std::cerr << "Error: --init and --promote do not go together\n";
    return 1;
  }
  if (initMode) {
    return initBeacon(workDir, genesisKeys);
  } else {
    return runBeacon(workDir, approvedRewindGeneration, promote);
  }
}
