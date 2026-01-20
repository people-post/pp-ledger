#include "Service.h"

namespace pp {

Service::~Service() {
  if (isRunning_) {
    stop();
  }
}

bool Service::start() {
  if (isRunning_) {
    log().warning << "Service is already running";
    return false;
  }

  // Call pre-start hook
  if (!onStart()) {
    log().error << "Service onStart() failed";
    return false;
  }

  isRunning_ = true;
  thread_ = std::thread(&Service::run, this);

  log().info << "Service started";
  return true;
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
