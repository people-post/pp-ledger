#include "../server/MinerServer.h"
#include "../lib/Logger.h"

#include <CLI/CLI.hpp>

#include <csignal>
#include <iostream>
#include <string>

namespace {
pp::MinerServer* g_minerServer = nullptr;

void signalHandler(int signal) {
  if (signal == SIGINT && g_minerServer) {
    g_minerServer->setStop(true);
  }
}
} // namespace

int runMiner(const std::string& workDir) {
  auto logger = pp::logging::getLogger("pp");
  
  logger.info << "Running miner with work directory: " << workDir;

  pp::MinerServer miner;
  miner.redirectLogger("pp.M");
  
  // Set global pointer for signal handler
  g_minerServer = &miner;
  
  auto runResult = miner.run(workDir);
  
  // Clear global pointer
  g_minerServer = nullptr;
  
  if (!runResult) {
    logger.error << "Failed to run miner: " + runResult.error().message;
    return 1;
  }

  logger.info << "Miner stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-miner - Miner server for pp-ledger"};
  
  std::string workDir;
  app.add_option("-d,--work-dir", workDir, "Work directory (required)")
      ->required();

  bool debugMode = false;
  app.add_flag("--debug", debugMode, "Enable debug logging (default: warning level)");

  app.footer(
    "Example:\n"
    "  pp-miner -d /path/to/work-dir [--debug]\n"
    "\n"
    "The miner will automatically create a default config.json if it doesn't exist.\n"
    "Config file format (config.json):\n"
    "  {\n"
    "    \"minerId\": \"miner1\",\n"
    "    \"stake\": 1000000,\n"
    "    \"host\": \"localhost\",           // Optional, default: localhost\n"
    "    \"port\": 8518,                    // Optional, default: 8518\n"
    "    \"beacons\": [\"127.0.0.1:8517\"]  // Required, at least one beacon\n"
    "  }"
  );

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel = debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  return runMiner(workDir);
}
