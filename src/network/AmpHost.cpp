#include "AmpHost.h"
#include "LedgerRpcProtocol.h"

namespace pp {
namespace network {

std::shared_ptr<InFlightLimit> makeInFlightLimit() {
  return std::make_shared<InFlightLimit>(pp::ledger::rpc::kMaxReceivingPerPeer, pp::ledger::rpc::kReservedForDuties,
                                         pp::ledger::rpc::kMaxWaitingPerPeer);
}

std::shared_ptr<InFlightLimit> makeBulkInFlightLimit() {
  return std::make_shared<InFlightLimit>(pp::ledger::rpc::kMaxBulkReceivingPerPeer,
                                         pp::ledger::rpc::kBulkReservedForDuties,
                                         pp::ledger::rpc::kMaxBulkWaitingPerPeer);
}

AmpHost::AmpHost() : gate_(std::make_shared<Gate>()) {}

AmpHost::~AmpHost() { gate_->setOpen(false); }

void AmpHost::open() {
  in_flight_ = makeInFlightLimit();
  bulk_in_flight_ = makeBulkInFlightLimit();
  gate_->setOpen(true);
}

void AmpHost::close() { gate_->setOpen(false); }

bool AmpHost::Gate::enter() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!open_) {
    return false;
  }
  ++inside_;
  return true;
}

void AmpHost::Gate::leave() {
  {
    std::lock_guard<std::mutex> lock(mu_);
    --inside_;
  }
  left_.notify_all();
}

void AmpHost::Gate::setOpen(bool open) {
  std::unique_lock<std::mutex> lock(mu_);
  open_ = open;
  if (!open) {
    left_.wait(lock, [this]() { return inside_ == 0; });
  }
}

} // namespace network
} // namespace pp
