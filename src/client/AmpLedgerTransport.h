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
  /** Runs a callable with exclusive access to `links` (empty: run inline). */
  using IoExclusive = std::function<void(const std::function<void()>&)>;

  AmpLedgerTransport(pp::amp::PeerLinkManager& links, std::string peer_key, IoPump io_pump = {},
                     IoExclusive io_exclusive = {});

  void setPeerKey(std::string peer_key) { peer_key_ = std::move(peer_key); }
  const std::string& peerKey() const { return peer_key_; }

  pp::amp::PeerLinkManager& links() { return links_; }
  const pp::amp::PeerLinkManager& links() const { return links_; }

  void setIoPump(IoPump pump) { io_pump_ = std::move(pump); }

  /** RegisterEndpoint under the exclusive section. */
  bool registerEndpoint(const std::string& peer_key, const std::string& multiaddr);

  Roe<std::string> roundTrip(const std::string& requestBody, std::chrono::milliseconds timeout) override;

private:
  void exclusive(const std::function<void()>& fn) const;

  pp::amp::PeerLinkManager& links_;
  std::string peer_key_;
  IoPump io_pump_;
  IoExclusive io_exclusive_;
};

} // namespace pp
