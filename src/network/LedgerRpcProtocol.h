#pragma once

#include "amp/L3/ChannelPolicy.h"

#include <chrono>
#include <cstddef>

namespace pp {
namespace ledger {
namespace rpc {

/** L4 protocol id for blockchain RPC over AMP (see docs/amp-transport.md). */
inline constexpr const char* kProtocolId = "/pp-ledger/rpc/1.0.0";

/** Max unframed request/response body (matches public TCP SecurityConfig default). */
inline constexpr size_t kMaxPayloadBytes = 512 * 1024;

inline constexpr std::chrono::milliseconds kDefaultReadTimeout{8000};

/** Control channel: one request/response per open (read_once). */
inline pp::amp::ChannelPolicy LedgerRpcChannelPolicy(
    std::chrono::milliseconds read_timeout = kDefaultReadTimeout) {
  pp::amp::ChannelPolicy policy;
  policy.cls = pp::amp::ChannelClass::Control;
  policy.drop = pp::amp::ChannelDropPolicy::Never;
  policy.max_outbound_frames = pp::amp::AmpChannelLimits::kMaxControlOutboundFrames;
  policy.read_once = true;
  policy.max_message_bytes = kMaxPayloadBytes;
  policy.read_timeout = read_timeout;
  return policy;
}

} // namespace rpc
} // namespace ledger
} // namespace pp
