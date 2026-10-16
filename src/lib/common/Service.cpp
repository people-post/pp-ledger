#include "Service.h"

namespace pp {

Service::~Service() {
  stop();
}

Service::Roe<void> Service::start() {
  stop();
  // Cleared before onStart(): a stop requested while it runs (a long startup sync) is kept.
  isStopSet_ = false;

  // Call pre-start hook
  auto result = onStart();
  if (!result) {
    return Error(-2, "Service onStart() failed: " + result.error().message);
  }
  if (isStopSet_) {
    log().info << "Stop requested during start: not running";
    return {};
  }

  thread_ = std::thread(&Service::runLoop, this);

  log().info << "Service started";
  return {};
}

void Service::stop() {
  isStopSet_ = true;
  if (thread_.joinable()) {
    thread_.join();
    onStop(); // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

Service::Roe<void> Service::run() {
  stop();

  // Cleared before onStart(): a stop requested while it runs (a long startup sync, a SIGTERM from
  // a service manager) is kept, and the run loop is skipped.
  isStopSet_ = false;
  auto result = onStart();
  if (!result) {
    return Error(-2, "Service onStart() failed: " + result.error().message);
  }

  if (isStopSet_) {
    log().info << "Stop requested during start: not running";
    isStopSet_ = true;
    onStop();
    return {};
  }
  log().info << "Service running in current thread";
  runLoop();
  isStopSet_ = true;
  onStop();
  log().info << "Service stopped (current thread)";
  return {};
}

} // namespace pp
