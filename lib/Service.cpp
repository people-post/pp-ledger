#include "Service.h"

namespace pp {

Service::~Service() {
  if (isRunning_) {
    stop();
  }
}

Service::Roe<void> Service::start() {
  if (isRunning_) {
    return Error(-1, "Service is already running");
  }

  // Call pre-start hook
  auto result = onStart();
  if (!result) {
    return Error(-2, "Service onStart() failed: " + result.error().message);
  }

  isRunning_ = true;
  thread_ = std::thread(&Service::run, this);

  log().info << "Service started";
  return {};
}

void Service::stop() {
  if (!isRunning_) {
    log().warning << "Service is not running";
    return;
  }

  log().info << "Stopping service";

  isRunning_ = false;

  if (thread_.joinable()) {
    thread_.join();
  }

  // Call post-stop hook
  onStop();

  log().info << "Service stopped";
}

} // namespace pp
