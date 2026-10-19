#include "common/Logger.h"
#include "../server/RoleRunner.h"

#include "NodeOptions.h"

#include <cli11.hpp>

#include <atomic>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>

namespace {
/** Set while the role runs: a signal stops it (one atomic store). */
std::atomic<pp::RoleRunner *> g_runner{nullptr};

void signalHandler(int signal) {
  if (signal == SIGINT || signal == SIGTERM) {
    if (pp::RoleRunner *runner = g_runner.load()) {
      runner->stop();
    }
  }
}
} // namespace

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

  pp::RoleRunner::Options run;
  run.role = pp::RoleRunner::Role::Relay;
  run.workDir = options.workDir;
  run.approvedRewindGeneration = approvedRewindGeneration;
  run.logFile = options.logFile;
  pp::RoleRunner runner(std::move(run));
  runner.redirectLogger("pp");
  g_runner = &runner;
  auto ran = runner.run();
  g_runner = nullptr;
  if (!ran) {
    pp::logging::getLogger("pp").error << ran.error().message;
    return 1;
  }
  return 0;
}
