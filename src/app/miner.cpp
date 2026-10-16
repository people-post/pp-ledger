#include "common/Logger.h"
#include "../server/MinerServer.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <iostream>
#include <string>

namespace {
pp::MinerServer *g_minerServer = nullptr;

/** Set by a signal: no restart follows the current run (beacon-authority bp1). */
std::atomic<bool> g_stopRequested{false};

void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_stopRequested = true;
    if (g_minerServer) {
      g_minerServer->setStop(true);
    }
  }
}
} // namespace

int runMiner(const std::string &workDir) {
  auto logger = pp::logging::getLogger("pp");

  logger.info << "Running miner with work directory: " << workDir;

  // A rewind is applied by a restart (beacon-authority bp1, B022): run a fresh server on the same
  // work directory for as long as the last one asks to be restarted.
  while (!g_stopRequested) {
    pp::MinerServer miner;
    miner.redirectLogger("pp.M");
    g_minerServer = &miner; // for the signal handler

    auto runResult = miner.run(workDir);
    g_minerServer = nullptr;

    if (!runResult) {
      logger.error << "Failed to run miner: " + runResult.error().message;
      return 1;
    }
    if (!miner.restartRequested()) {
      break;
    }
    logger.warning << "Miner restarting";
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
  app.add_flag("--debug", debugMode,
               "Enable debug logging (default: warning level)");

  app.footer(
      "Example:\n"
      "  pp-miner -d /path/to/work-dir [--debug]\n"
      "\n"
      "The miner will automatically create a default config.json if it doesn't "
      "exist.\n");

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel =
      debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Ctrl+C, and the SIGTERM a service manager (docker, systemd) stops with: stop cleanly, so the
  // ledger commits what it appended.
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  return runMiner(workDir);
}
