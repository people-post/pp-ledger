#pragma once

#include "amp/link/PeerLinkManager.h"

#include <functional>
#include <string>

namespace pp {
namespace network {

/**
 * Registers /pp-ledger/rpc/1.0.0 on a PeerLinkManager.
 * Handler receives unframed binaryPack(Client::Request) bytes and returns unframed response bytes.
 */
class AmpLedgerServer {
public:
  using Handler = std::function<std::string(const std::string& requestBody)>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Queue work onto the Amp IO / pump thread (MeshRuntime::PostToIo). */
  using IoPost = std::function<void(std::function<void()>)>;
  /** Return true to accept an inbound channel from this remote PeerId. Empty (default): allow all. */
  using PeerAllowed = std::function<bool(const std::string& remotePeerId)>;

  static void Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker = {},
                   IoPost post_io = {}, PeerAllowed peer_allowed = {});

  static void Unbind(pp::amp::PeerLinkManager& links);
};

} // namespace network
} // namespace pp
