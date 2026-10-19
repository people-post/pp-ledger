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

AmpHost::AmpHost() = default;

AmpHost::~AmpHost() { close(); }

void AmpHost::open() {
  in_flight_ = makeInFlightLimit();
  bulk_in_flight_ = makeBulkInFlightLimit();
  open_->store(true, std::memory_order_release);
}

void AmpHost::close() {
  open_->store(false, std::memory_order_release);
  lifetime_.InvalidateAndWait();  // a call that saw it open finishes first (not one on this thread)
}

} // namespace network
} // namespace pp
