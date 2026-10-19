#include "ledger/server/RoleRunner.h"

#include "server/BeaconServer.h"
#include "server/MinerServer.h"
#include "server/RelayServer.h"
#include "server/RoleLoop.h"

namespace pp {

namespace {

std::unique_ptr<Server> makeRoleServer(const RoleRunner::Options &options, const bool firstStart) {
  switch (options.role) {
  case RoleRunner::Role::Beacon: {
    auto beacon = std::make_unique<BeaconServer>();
    beacon->redirectLogger("pp.B");
    // The first start promotes and accepts the ledger's tip; a restart (a rewind) runs the beacon it became.
    beacon->setPromote(firstStart && options.promote);
    beacon->setAcceptLedgerTip(firstStart && options.acceptLedgerTip);
    return beacon;
  }
  case RoleRunner::Role::Relay: {
    auto relay = std::make_unique<RelayServer>();
    relay->redirectLogger("pp.R");
    return relay;
  }
  case RoleRunner::Role::Miner: {
    auto miner = std::make_unique<MinerServer>();
    miner->redirectLogger("pp.M");
    return miner;
  }
  }
  return nullptr;
}

} // namespace

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

Service::Roe<void> RoleRunner::run() {
  if (options_.host && options_.role == Role::Beacon) {
    return Service::Error(-1, "A beacon runs standalone only, not on a host's runtime");
  }
  return runRoleLoop(
      options_, nameOf(options_.role),
      [this](const bool firstStart) { return makeRoleServer(options_, firstStart); }, stop_, log());
}

} // namespace pp
