#pragma once

#include "amp/link/Types.h"

#include <chrono>
#include <cstddef>

namespace pp {
namespace network {

/**
 * pp-ledger's network policy, fixed in code (not operator config): every
 * deadline derives from one RPC timeout so they cannot drift apart, and the
 * amp transport policy is passed through. Tests may override fields.
 */
struct NetworkTuning {
  static constexpr std::chrono::milliseconds kDefaultRpcTimeout{15000};
  static constexpr size_t kDefaultRequestQueueCapacity = 1024;
  static constexpr std::chrono::milliseconds kDefaultStartupSyncTimeout{5 * 60 * 1000};

  /** How long a client waits for a light reply; data requests wait twice this. */
  std::chrono::milliseconds rpcTimeout{kDefaultRpcTimeout};
  /** Queued requests beyond this are refused with a busy reply. */
  size_t requestQueueCapacity{kDefaultRequestQueueCapacity};
  /** How long a relay / miner waits to catch up with its upstream at start. */
  std::chrono::milliseconds startupSyncTimeout{kDefaultStartupSyncTimeout};
  /** Transport policy passed to amp (docs/TUNING.md in pp-cpp-amp). */
  pp::adp::AdpTuning adp;
  pp::amp::MuxTuning mux;

  /**
   * A request queued longer than this is answered "expired" without doing
   * the work: by the time a reply crosses back, the client has given up.
   */
  std::chrono::milliseconds serverQueueExpiry() const { return rpcTimeout / 2; }

  /** How long an upstream holds a BLOCK_WAIT with no new block before answering (the client waits T). */
  std::chrono::milliseconds blockWaitHold() const { return rpcTimeout / 2; }

  /** Data requests (blocks, transactions, accounts) wait twice the RPC timeout. */
  static constexpr std::chrono::milliseconds dataTimeoutFor(std::chrono::milliseconds rpc) { return rpc * 2; }

  /**
   * Read timeout of an inbound RPC channel (request in, reply out): the
   * longest any client waits. Past it nobody is listening for the reply, and a
   * peer that opened a channel and never sent is dropped.
   */
  std::chrono::milliseconds channelReadTimeout() const { return dataTimeoutFor(rpcTimeout); }

  /** Copy the amp policy into a link config. */
  void applyTo(pp::amp::PeerLinkConfig &config) const;

};

} // namespace network
} // namespace pp
