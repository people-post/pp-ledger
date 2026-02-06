#include "../lib/Logger.h"
#include "../server/RelayServer.h"

#include <CLI/CLI.hpp>

#include <csignal>
#include <iostream>
#include <string>

namespace {
pp::RelayServer *g_relayServer = nullptr;

void signalHandler(int signal) {
  if (signal == SIGINT && g_relayServer) {
    g_relayServer->setStop(true);
  }
}
} // namespace

int runRelay(const std::string &workDir) {
  auto logger = pp::logging::getLogger("pp");

  logger.info << "Running relay with work directory: " << workDir;

  pp::RelayServer relay;
  relay.redirectLogger("pp.R");

  // Set global pointer for signal handler
  g_relayServer = &relay;

  auto runResult = relay.run(workDir);

  // Clear global pointer
  g_relayServer = nullptr;

  if (!runResult) {
    logger.error << "Failed to run relay: " + runResult.error().message;
    return 1;
  }

  logger.info << "Relay stopped";
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-relay - Relay server for pp-ledger"};

  std::string workDir;
  app.add_option("-d,--work-dir", workDir, "Work directory (required)")
      ->required();

  bool debugMode = false;
  app.add_flag("--debug", debugMode,
               "Enable debug logging (default: warning level)");

  app.footer(
      "Example:\n"
      "  pp-relay -d /path/to/work-dir [--debug]\n"
      "\n"
      "The relay will automatically create a default config.json if it doesn't "
      "exist.\n");

  CLI11_PARSE(app, argc, argv);

  auto logger = pp::logging::getRootLogger();
  pp::logging::Level logLevel =
      debugMode ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING;
  logger.setLevel(logLevel);
  logger.info << "Logging level set to " << (debugMode ? "DEBUG" : "WARNING");

  // Set up signal handler for Ctrl+C
  std::signal(SIGINT, signalHandler);

  return runRelay(workDir);
}
