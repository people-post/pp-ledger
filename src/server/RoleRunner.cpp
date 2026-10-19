#include "ledger/server/RoleRunner.h"

#include "ledger/server/BeaconServer.h"
#include "ledger/server/MinerServer.h"
#include "ledger/server/RelayServer.h"

namespace pp {

std::optional<RoleRunner::Role> RoleRunner::roleNamed(const std::string &name) {
  if (name == "beacon") {
    return Role::Beacon;
  }
  if (name == "relay") {
    return Role::Relay;
  }
  if (name == "miner") {
    return Role::Miner;
  }
  return std::nullopt;
}

std::string RoleRunner::nameOf(const Role role) {
  switch (role) {
  case Role::Beacon:
    return "beacon";
  case Role::Relay:
    return "relay";
  case Role::Miner:
    return "miner";
  }
  return "?";
}

RoleRunner::RoleRunner(Options options) : options_(std::move(options)) { redirectLogger("RoleRunner"); }

std::unique_ptr<Server> RoleRunner::makeServer(const bool firstStart) const {
  if (options_.makeServer) {
    return options_.makeServer();
  }
  switch (options_.role) {
  case Role::Beacon: {
    auto beacon = std::make_unique<BeaconServer>();
    beacon->redirectLogger("pp.B");
    // The first start promotes and accepts the ledger's tip; a restart (a rewind) runs the beacon it became.
    beacon->setPromote(firstStart && options_.promote);
    beacon->setAcceptLedgerTip(firstStart && options_.acceptLedgerTip);
    return beacon;
  }
  case Role::Relay: {
    auto relay = std::make_unique<RelayServer>();
    relay->redirectLogger("pp.R");
    return relay;
  }
  case Role::Miner: {
    auto miner = std::make_unique<MinerServer>();
    miner->redirectLogger("pp.M");
    return miner;
  }
  }
  return nullptr;
}

Service::Roe<void> RoleRunner::run() {
  const std::string role = nameOf(options_.role);
  if (options_.host && options_.role == Role::Beacon && !options_.makeServer) {
    return Service::Error(-1, "A beacon runs standalone only, not on a host's runtime");
  }
  log().info << "Running " << role << " with work directory: " << options_.workDir
             << (options_.host ? " (on the host's Amp runtime)" : "");
  bool firstStart = true;
  while (!stop_) {
    auto server = makeServer(firstStart);
    firstStart = false;
    server->setStopSource(&stop_);
    server->setApprovedRewindGeneration(options_.approvedRewindGeneration);
    server->setLogFile(options_.logFile);
    server->setOnListening(options_.onListening);
    if (options_.host) {
      server->runOnHost(*options_.host);
    }
    if (auto ran = server->run(options_.workDir); !ran) {
      return Service::Error(-1, "Failed to run " + role + ": " + ran.error().message);
    }
    if (!server->restartRequested()) {
      break;
    }
    log().warning << "The " << role << " restarts";
  }
  log().info << "The " << role << " stopped";
  return {};
}

} // namespace pp
