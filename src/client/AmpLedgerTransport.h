#pragma once

#include "ILedgerTransport.h"
#include "amp/link/PeerLinkManager.h"
#include "common/Module.h"

#include <functional>
#include <string>

namespace pp {

/** AMP channel transport for ledger RPC (unframed binaryPack on L3 DATA). */
class AmpLedgerTransport : public ILedgerTransport, public Module {
public:
  using IoPump = std::function<void()>;

  AmpLedgerTransport(pp::amp::PeerLinkManager& links, std::string peer_key, IoPump io_pump = {});

  void setPeerKey(std::string peer_key) { peer_key_ = std::move(peer_key); }
  const std::string& peerKey() const { return peer_key_; }

  pp::amp::PeerLinkManager& links() { return links_; }
  const pp::amp::PeerLinkManager& links() const { return links_; }

  void setIoPump(IoPump pump) { io_pump_ = std::move(pump); }

  Roe<std::string> roundTrip(const std::string& requestBody, std::chrono::milliseconds timeout) override;

private:
  pp::amp::PeerLinkManager& links_;
  std::string peer_key_;
  IoPump io_pump_;
};

} // namespace pp
