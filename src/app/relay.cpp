#include "common/Logger.h"
#include "../server/RelayServer.h"

#include "NodeOptions.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
pp::RelayServer *g_relayServer = nullptr;

/** Set by a signal: no restart follows the current run (beacon-authority bp1). */
std::atomic<bool> g_stopRequested{false};

void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    g_stopRequested = true;
    if (g_relayServer) {
      g_relayServer->setStop(true);
    }
  }
}
} // namespace

int runRelay(const std::string &workDir, uint64_t approvedRewindGeneration, const std::string &logFile) {
  auto logger = pp::logging::getLogger("pp");

  logger.info << "Running relay with work directory: " << workDir;

  // A rewind is applied by a restart (beacon-authority bp1, B022): run a fresh server on the same
  // work directory for as long as the last one asks to be restarted.
  while (!g_stopRequested) {
    pp::RelayServer relay;
    relay.redirectLogger("pp.R");
    relay.setApprovedRewindGeneration(approvedRewindGeneration);
    relay.setLogFile(logFile);
    g_relayServer = &relay; // for the signal handler

    auto runResult = relay.run(workDir);
    g_relayServer = nullptr;

    if (!runResult) {
      logger.error << "Failed to run relay: " + runResult.error().message;
      return 1;
    }
    if (!relay.restartRequested()) {
      break;
    }
    logger.warning << "Relay restarting";
  }

  logger.info << "Relay stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-relay - Relay server for pp-ledger"};

  pp::app::NodeOptions options;
  options.add(app);

  uint64_t approvedRewindGeneration = 0;
  app.add_option("--approve-rewind", approvedRewindGeneration,
                 "Apply the rewind ordered by this beacon generation although it reaches back further "
                 "than a node rewinds on its own (7 days of slots); check the certificate first");

  app.footer(
      "Example:\n"
      "  pp-relay -d /path/to/work-dir [--debug]\n"
      "\n"
      "The relay will automatically create a default config.json if it doesn't "
      "exist.\n");

  CLI11_PARSE(app, argc, argv);

  options.applyLogLevel();

  // Ctrl+C, and the SIGTERM a service manager (docker, systemd) stops with: stop cleanly, so the
  // ledger commits what it appended.
  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);

  return runRelay(options.workDir, approvedRewindGeneration, options.logFile);
}
