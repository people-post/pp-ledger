#include "Relay.h"
#include "common/Logger.h"

namespace pp {

std::ostream &operator<<(std::ostream &os, const Relay::InitConfig &config) {
  os << "InitConfig{workDir=\"" << config.workDir << "\", "
     << "timeOffsetMs=" << config.timeOffsetMs << ", "
     << "startingBlockId=" << config.startingBlockId << "}";
  return os;
}

Relay::Relay() : FollowerCore("Relay") {}

Relay::Roe<void> Relay::init(const InitConfig &config) {
  log().info << "Initializing Relay";
  log().debug << "Init config: " << config;
  if (auto opened = openLedger(config.workDir, config.timeOffsetMs, config.startingBlockId); !opened) {
    return opened;
  }
  log().info << "Relay initialized successfully";
  log().info << "  Starting block ID: " << config.startingBlockId;
  log().info << "  Next block ID: " << getNextBlockId();
  return {};
}

} // namespace pp
