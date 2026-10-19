#include "server/RoleLoop.h"

namespace pp {

Service::Roe<void> runRoleLoop(const RoleRunner::Options &options, const std::string &role,
                               const RoleServerFactory &make, const std::atomic<bool> &stop,
                               logging::Logger &log) {
  log.info << "Running " << role << " with work directory: " << options.workDir
           << (options.host ? " (on the host's Amp runtime)" : "");
  bool firstStart = true;
  while (!stop) {
    auto server = make(firstStart);
    firstStart = false;
    server->setStopSource(&stop);
    server->setApprovedRewindGeneration(options.approvedRewindGeneration);
    server->setLogFile(options.logFile);
    server->setOnListening(options.onListening);
    if (options.host) {
      server->runOnHost(*options.host);
    }
    if (auto ran = server->run(options.workDir); !ran) {
      return Service::Error(-1, "Failed to run " + role + ": " + ran.error().message);
    }
    if (!server->restartRequested()) {
      break;
    }
    log.warning << "The " << role << " restarts";
  }
  log.info << "The " << role << " stopped";
  return {};
}

} // namespace pp
