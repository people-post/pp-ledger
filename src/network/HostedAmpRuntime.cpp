#include "ledger/network/HostedAmpRuntime.h"

namespace pp {
namespace network {

HostedAmpRuntime::HostedAmpRuntime(Host host) : host_(std::move(host)), io_(host_.runtime->IoExecutor()) {
  open();
  open_ = true;
}

HostedAmpRuntime::~HostedAmpRuntime() { closeHosted(); }

void HostedAmpRuntime::closeHosted() {
  if (!open_.exchange(false)) {
    return;
  }
  close();
}

void HostedAmpRuntime::post(IoTask task) {
  if (open_) {
    io_->Post(guardTask(std::move(task)));
  }
}

bool HostedAmpRuntime::onIoThread() const { return io_->RunsTasksOnCurrentThread(); }

std::string HostedAmpRuntime::listenMultiaddr() const {
  return open_ && host_.listenMultiaddr ? host_.listenMultiaddr() : std::string{};
}

} // namespace network
} // namespace pp
