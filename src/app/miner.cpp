#include "common/Logger.h"
#include "../server/MinerServer.h"

#include "NodeOptions.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
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

int runMiner(const std::string &workDir, uint64_t approvedRewindGeneration) {
  auto logger = pp::logging::getLogger("pp");

  logger.info << "Running miner with work directory: " << workDir;

  // A rewind is applied by a restart (beacon-authority bp1, B022): run a fresh server on the same
  // work directory for as long as the last one asks to be restarted.
  while (!g_stopRequested) {
    pp::MinerServer miner;
    miner.redirectLogger("pp.M");
    miner.setApprovedRewindGeneration(approvedRewindGeneration);
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

  pp::app::NodeOptions options;
  options.add(app);

  uint64_t approvedRewindGeneration = 0;
  app.add_option("--approve-rewind", approvedRewindGeneration,
                 "Apply the rewind ordered by this beacon generation although it reaches back further "
                 "than a node rewinds on its own (7 days of slots); check the certificate first");

  app.footer(
      "Example:\n"
      "  pp-miner -d /path/to/work-dir [--debug]\n"
      "\n"
      "The miner will automatically create a default config.json if it doesn't "
      "exist.\n");

  CLI11_PARSE(app, argc, argv);

  options.applyLogLevel();

  // Ctrl+C, and the SIGTERM a service manager (docker, systemd) stops with: stop cleanly, so the
  // ledger commits what it appended.
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  return runMiner(options.workDir, approvedRewindGeneration);
}
