#pragma once

#include "LedgerAmpRuntime.h"
#include "amp/L2/Types.h"
#include "amp/link/Types.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Meta.h"

#include <string>

namespace pp {
namespace network {

/** Build link config with fleet-compatible PeerId derivation. */
pp::amp::PeerLinkConfig DefaultLedgerLinkConfig();

/** ML-DSA identity + peer id from a raw ML-DSA-65 secret key (4032 bytes). */
pp::Roe<LedgerAmpConfig> LedgerAmpConfigFromPrivateKey(const std::string& private_key_raw, uint16_t udp_port);

/** Parse beacon entry: ADP multiaddr string or legacy {host, port, peerId}. */
pp::Roe<std::string> ParseBeaconMultiaddr(const pp::common::Object& entry);
pp::Roe<std::string> ParseBeaconMultiaddrString(const std::string& value);

} // namespace network
} // namespace pp
