#include "Service.h"

namespace pp {

Service::~Service() {
  if (!isStopSet_) {
    stop();
  }
}

Service::Roe<void> Service::start() {
  if (!isStopSet_) {
    return Error(-1, "Service is already running");
  }

  // Call pre-start hook
  auto result = onStart();
  if (!result) {
    return Error(-2, "Service onStart() failed: " + result.error().message);
  }

  isStopSet_ = false;
  thread_ = std::thread(&Service::runLoop, this);

  log().info << "Service started";
  return {};
}

void Service::stop() {
  if (isStopSet_) {
    log().warning << "Service is not running";
    return;
  }

  log().info << "Stopping service";

  isStopSet_ = true;

  if (thread_.joinable()) {
    thread_.join();
  }

  // Call post-stop hook
  onStop();

  log().info << "Service stopped";
}

Service::Roe<void> Service::run() {
  if (!isStopSet_) {
    return Error(-1, "Service is already running");
  }

  auto result = onStart();
  if (!result) {
    return Error(-2, "Service onStart() failed: " + result.error().message);
  }

  isStopSet_ = false;
  log().info << "Service running in current thread";
  runLoop();
  isStopSet_ = true;
  onStop();
  log().info << "Service stopped (current thread)";
  return {};
}

} // namespace pp
