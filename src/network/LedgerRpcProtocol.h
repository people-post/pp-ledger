#pragma once

#include "amp/L3/AmpChannelLimits.h"
#include "amp/L3/ChannelPolicy.h"

#include <chrono>
#include <cstddef>

namespace pp {
namespace ledger {
namespace rpc {

/** L4 protocol id for blockchain RPC over AMP (see docs/contracts/AMP_TRANSPORT.md). */
inline constexpr const char* kProtocolId = "/pp-ledger/rpc/1.0.0";

/**
 * Max request/response body: what Amp's Bulk profile carries in one message — a block of the
 * largest `maxBlockBytes` any chain config may set (4 MiB, chain_tx::kMaxBlockBytesCeiling) plus a
 * 64 KiB envelope (B034). The chain sets block sizes; the transport carries them.
 */
inline constexpr size_t kMaxMessageBytes = pp::amp::AmpChannelLimits::kMaxBulkFrameBytes;
/** The envelope around a block or record in a message (request / response headers, a tip). */
inline constexpr size_t kEnvelopeBytes = 64 * 1024;
/** Requests that carry neither a block nor a record (every type but BLOCK_ADD and TX_ADD). */
inline constexpr size_t kMaxSmallRequestBytes = 64 * 1024;
/**
 * Channels a peer may have receiving a request at once (opened, body not yet complete): bounds
 * what one peer makes a node buffer to this many × kMaxMessageBytes.
 */
inline constexpr size_t kMaxReceivingPerPeer = 16;

/**
 * Bulk channel: one request/response per open (read_once). amp resets it when nothing arrives for
 * `read_timeout`, which covers the wait for the reply.
 */
inline pp::amp::ChannelPolicy LedgerRpcChannelPolicy(std::chrono::milliseconds read_timeout) {
  pp::amp::ChannelPolicy policy = pp::amp::MakeBulkChannelPolicy();
  policy.read_once = true;
  policy.max_message_bytes = kMaxMessageBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

} // namespace rpc
} // namespace ledger
} // namespace pp
