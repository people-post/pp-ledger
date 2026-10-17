#pragma once

#include "LedgerRpcProtocol.h"
#include "NetworkTuning.h"
#include "amp/link/PeerLinkManager.h"

#include <chrono>
#include <functional>
#include <string>

namespace pp {
namespace network {

/**
 * Registers /pp-ledger/rpc/1.0.0 (requests up to kMaxSmallRequestBytes) and /pp-ledger/rpc-bulk/1.0.0
 * (larger ones) on a PeerLinkManager.
 * Requests are unframed binaryPack(Client::Request) bytes; replies are unframed response bytes.
 */
class AmpLedgerServer {
public:
  /** Sends the reply for one inbound request. Any thread; only the first call counts. */
  using Reply = std::function<void(std::string response)>;
  /**
   * Receives a request on the io lane; replies now or later via `reply`.
   * `remotePeerId` is the sender's authenticated AMP peer id.
   */
  using AsyncHandler = std::function<void(std::string remotePeerId, std::string requestBody, Reply reply)>;

  using Handler = std::function<std::string(const std::string& requestBody)>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Queue work onto the Amp IO / pump thread (MeshRuntime::PostToIo). */
  using IoPost = std::function<void(std::function<void()>)>;

  /** Default inbound channel read timeout (NetworkTuning::channelReadTimeout at default settings). */
  static constexpr std::chrono::milliseconds kDefaultReadTimeout =
      NetworkTuning::dataTimeoutFor(NetworkTuning::kDefaultRpcTimeout);

  /** Why a request was refused when its channel opened, before its body was read. */
  enum class Refusal {
    NotAllowed, ///< a bulk request from a peer `admit` refuses
    PeerBusy,   ///< the peer already has its share of requests arriving
    NodeBusy,   ///< bulk requests from all peers fill the node's share
  };

  struct Options {
    IoPost post_io;
    std::chrono::milliseconds read_timeout{kDefaultReadTimeout};
    /** Whether a peer is served large requests, asked when the channel opens; empty: every peer. */
    std::function<bool(const std::string& remotePeerId)> admit;
    /** The reply to a refused request, sent at once; empty: the channel is reset. */
    std::function<std::string(Refusal)> refusal;
    /** Large requests arriving at once, from one peer and from all. */
    size_t bulkPerPeer{pp::ledger::rpc::kMaxBulkReceivingPerPeer};
    size_t bulkTotal{pp::ledger::rpc::kMaxBulkReceivingTotal};
  };

  static void BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, Options options);
  static void BindAsync(pp::amp::PeerLinkManager& links, AsyncHandler handler, IoPost post_io = {},
                        std::chrono::milliseconds read_timeout = kDefaultReadTimeout);

  /** Synchronous handler, run inline on the io lane or via `post_worker`. */
  static void Bind(pp::amp::PeerLinkManager& links, Handler handler, WorkerPost post_worker = {},
                   IoPost post_io = {}, std::chrono::milliseconds read_timeout = kDefaultReadTimeout);

  static void Unbind(pp::amp::PeerLinkManager& links);
};

} // namespace network
} // namespace pp
