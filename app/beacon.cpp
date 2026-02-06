#include "../server/BeaconServer.h"
#include "../server/Beacon.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include <CLI/CLI.hpp>

#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>

namespace {
pp::BeaconServer* g_beaconServer = nullptr;

void signalHandler(int signal) {
  if (signal == SIGINT && g_beaconServer) {
    g_beaconServer->setStop(true);
  }
}
} // namespace

int initBeacon(const std::string& workDir) {
  pp::BeaconServer beaconServer;
  beaconServer.redirectLogger("pp.BeaconServer");
  
  auto result = beaconServer.init(workDir);
  if (!result) {
    std::cerr << "Error: Failed to initialize beacon: " << result.error().message << "\n";
    return 1;
  }
  
  std::cout << "Beacon initialized successfully (to reinitialize, edit the init config file and run the same command)\n";
  std::cout << "Please save the private keys, they are not recoverable: " << result.value().toJson().dump(2) << "\n";
  std::cout << "You can now start the beacon with: pp-beacon -d " << workDir << "\n";
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
  app.add_flag("--init", initMode, "Initialize a new beacon");

  bool debugMode = false;
  app.add_flag("--debug", debugMode, "Enable debug logging (default: warning level)");

  app.footer(
    "Mode 1: Mount existing beacon:\n"
    "  pp-beacon -d /path/to/work-dir [--debug]\n"
    "  The work directory must contain config.json\n"
    "\n"
    "Mode 2: Initialize new beacon:\n"
    "  pp-beacon -d /path/to/work-dir --init [--debug]\n"
    "  Creates init-config.json if it doesn't exist, then initializes the beacon\n"
    "\n"
    "Config file format (config.json):\n"
    "  {\n"
    "    \"host\": \"localhost\",           // Optional, default: localhost\n"
    "    \"port\": 8517,                    // Optional, default: 8517\n"
    "    \"whitelist\": [\"host:port\"],    // Optional, whitelisted beacons\n"
    "    \"checkpointSize\": 1073741824,    // Optional, default: 1GB\n"
    "    \"checkpointAge\": 31536000        // Optional, default: 1 year\n"
    "  }"
  );

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel = debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  if (initMode) {
    return initBeacon(workDir);
  } else {
    return runBeacon(workDir);
  }
}
