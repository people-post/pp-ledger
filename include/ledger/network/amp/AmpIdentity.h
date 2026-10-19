#pragma once

#include "ledger/network/LedgerAmpRuntime.h"
#include "amp/L2/Types.h"
#include "amp/link/Types.h"
#include "common/ResultOrError.hpp"
#include "ledger/lib/common/Meta.h"

#include <string>

namespace pp {
namespace network {

/** Build link config with fleet-compatible PeerId derivation. */
pp::amp::PeerLinkConfig DefaultLedgerLinkConfig();

/** ML-DSA identity + peer id from a raw ML-DSA-65 secret key (4032 bytes). */
pp::Roe<LedgerAmpConfig> LedgerAmpConfigFromPrivateKey(const std::string& private_key_raw, uint16_t udp_port);

/** The raw ML-DSA-65 public key of a raw private key (mldsa-native derives it). */
pp::Roe<std::string> PublicKeyFromPrivateKey(const std::string& private_key_raw);

/** Parse beacon entry: ADP multiaddr string or legacy {host, port, peerId}. */
pp::Roe<std::string> ParseBeaconMultiaddrString(const std::string& value);

} // namespace network
} // namespace pp
