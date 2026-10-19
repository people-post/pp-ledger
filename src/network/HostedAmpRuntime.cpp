#include "HostedAmpRuntime.h"

namespace pp {
namespace network {

HostedAmpRuntime::HostedAmpRuntime(Host host) : host_(std::move(host)) {
  // MeshRuntime::IsDriving is the driver's own state: learn the driver's thread from a tick instead.
  tick_ = host_.runtime->AddIoTick([driver = driver_]() { *driver = std::this_thread::get_id(); });
  open();
  open_ = true;
}

HostedAmpRuntime::~HostedAmpRuntime() { closeHosted(); }

void HostedAmpRuntime::closeHosted() {
  if (!open_.exchange(false)) {
    return;
  }
  close();
  host_.runtime->RemoveIoTick(tick_);
}

void HostedAmpRuntime::post(IoTask task) {
  if (open_) {
    host_.runtime->PostToIo(guardTask(std::move(task)));
  }
}

bool HostedAmpRuntime::onIoThread() const { return std::this_thread::get_id() == driver_->load(); }

std::string HostedAmpRuntime::listenMultiaddr() const {
  return open_ && host_.listenMultiaddr ? host_.listenMultiaddr() : std::string{};
}

} // namespace network
} // namespace pp
