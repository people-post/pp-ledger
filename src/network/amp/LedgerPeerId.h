#pragma once

#include "amp/L2/Types.h"
#include "common/ResultOrError.hpp"

#include <string>
#include <vector>

namespace pp {
namespace network {

/** libp2p-compatible PeerId (base58) from an ML-DSA-65 public key. */
pp::Roe<std::string> PeerIdFromMlDsaPublicKey(const std::vector<uint8_t>& public_key);

} // namespace network
} // namespace pp
