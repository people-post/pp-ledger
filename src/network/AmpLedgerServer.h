#pragma once

#include "amp/link/PeerLinkManager.h"

#include <functional>
#include <string>

namespace pp {
namespace network {

/**
 * Registers /pp-ledger/rpc/1.0.0 on a PeerLinkManager.
 * Requests are unframed binaryPack(Client::Request) bytes; replies are unframed response bytes.
 */
class AmpLedgerServer {
public:
  /** Sends the reply for one inbound request. Any thread; only the first call counts. */
  using Reply = std::function<void(std::string response)>;
  /** Receives a request on the io lane; replies now or later via `reply`. */
  using AsyncHandler = std::function<void(std::string requestBody, Reply reply)>;

  using Handler = std::function<std::string(const std::string& requestBody)>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Queue work onto the Amp IO / pump thread (MeshRuntime::PostToIo). */
  using IoPost = std::function<void(std::function<void()>)>;

  static void BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, IoPost post_io = {});

  /** Synchronous handler, run inline on the io lane or via `post_worker`. */
  static void Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker = {},
                   IoPost post_io = {});

  static void Unbind(pp::amp::PeerLinkManager& links);
};

} // namespace network
} // namespace pp
