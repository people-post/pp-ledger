#include "Service.h"

namespace pp {

Service::~Service() {
  stop();
}

Service::Roe<void> Service::start() {
  stop();

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
  isStopSet_ = true;
  if (thread_.joinable()) {
    thread_.join();
    onStop(); // NOLINT(clang-analyzer-optin.cplusplus.VirtualCall)
  }
}

Service::Roe<void> Service::run() {
  stop();

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
