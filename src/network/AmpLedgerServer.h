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

  static void Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker = {});

  static void Unbind(pp::amp::PeerLinkManager& links);
};

} // namespace network
} // namespace pp
