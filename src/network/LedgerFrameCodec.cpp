#include "LedgerFrameCodec.h"

#include <arpa/inet.h>
#include <cstring>

namespace pp {
namespace network {

LedgerFrameCodec::Roe<std::string> LedgerFrameCodec::encode(std::string_view payload) {
  if (payload.size() > MAX_PAYLOAD_SIZE) {
    return Error(3, "Frame too large: " + std::to_string(payload.size()));
  }

  std::string framed;
  framed.resize(sizeof(uint32_t) + payload.size());
  const uint32_t netLen = htonl(static_cast<uint32_t>(payload.size()));
  std::memcpy(framed.data(), &netLen, sizeof(netLen));
  if (!payload.empty()) {
    std::memcpy(framed.data() + sizeof(uint32_t), payload.data(), payload.size());
  }
  return framed;
}

LedgerFrameCodec::Roe<uint32_t>
LedgerFrameCodec::decodeLengthPrefix(const void *header, size_t headerSize) {
  if (headerSize < sizeof(uint32_t)) {
    return Error(1, "Frame header too short");
  }

  uint32_t netLen = 0;
  std::memcpy(&netLen, header, sizeof(netLen));
  const uint32_t len = ntohl(netLen);
  if (auto valid = validatePayloadLength(len); !valid) {
    return valid.error();
  }
  return len;
}

LedgerFrameCodec::Roe<void> LedgerFrameCodec::validatePayloadLength(uint32_t payloadLen) {
  if (payloadLen > MAX_PAYLOAD_SIZE) {
    return Error(2, "Frame too large: " + std::to_string(payloadLen));
  }
  return {};
}

} // namespace network
} // namespace pp
