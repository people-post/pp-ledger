#pragma once

#include "lib/common/ResultOrError.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace pp {
namespace network {

/**
 * Ledger RPC wire framing (TCP and libp2p /pp-ledger/rpc/1.0.0).
 *
 * Wire format: u32 BE payload length + payload bytes.
 * Payload is unframed ledger RPC envelope bytes (binaryPack Request/Response).
 */
struct LedgerFrameCodec {
  static constexpr uint32_t MAX_PAYLOAD_SIZE = 16 * 1024 * 1024; // 16 MiB

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  /** Encode unframed payload → length-prefixed frame. */
  static Roe<std::string> encode(std::string_view payload);

  /** Decode length prefix (4 bytes, network byte order). */
  static Roe<uint32_t> decodeLengthPrefix(const void *header, size_t headerSize);

  static Roe<void> validatePayloadLength(uint32_t payloadLen);
};

} // namespace network
} // namespace pp
