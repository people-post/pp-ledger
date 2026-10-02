#include "AmpIdentity.h"

#include "LedgerPeerId.h"
#include "amp/link/AdpMultiaddr.h"
#include "common/io/Json.h"
#include "crypto/MlDsa.h"
#include "lib/common/Utilities.h"

#include <mldsa_native.h>

#include <array>
#include <sstream>

namespace pp {
namespace network {
namespace {

pp::amp::MshIdentity IdentityFromPrivateKeyBytes(const std::string& private_key) {
  pp::amp::MshIdentity identity;
  if (private_key.size() != utl::kMlDsaPrivateKeyBytes) {
    return identity;
  }
  identity.ml_dsa_secret_key.assign(private_key.begin(), private_key.end());
  identity.ml_dsa_public_key.resize(utl::kMlDsaPublicKeyBytes);
  if (mldsa_pk_from_sk(identity.ml_dsa_public_key.data(),
                       reinterpret_cast<const uint8_t*>(private_key.data())) != 0) {
    identity.ml_dsa_public_key.clear();
  }
  return identity;
}

} // namespace

pp::amp::PeerLinkConfig DefaultLedgerLinkConfig() {
  pp::amp::PeerLinkConfig config;
  config.peer_id_from_identity = [](const pp::amp::ByteVector& identity_public_key) -> std::string {
    std::vector<uint8_t> bytes(identity_public_key.begin(), identity_public_key.end());
    auto peer_id = PeerIdFromMlDsaPublicKey(bytes);
    return peer_id ? *peer_id : std::string{};
  };
  return config;
}

pp::Roe<LedgerAmpConfig> LedgerAmpConfigFromPrivateKey(const std::string& private_key_raw,
                                                          uint16_t udp_port) {
  auto identity = IdentityFromPrivateKeyBytes(private_key_raw);
  if (identity.ml_dsa_public_key.size() != utl::kMlDsaPublicKeyBytes) {
    return pp::Error("failed to derive ML-DSA public key from secret key");
  }

  auto peer_id = PeerIdFromMlDsaPublicKey(identity.ml_dsa_public_key);
  if (!peer_id) {
    return peer_id.error();
  }

  LedgerAmpConfig config;
  config.identity = std::move(identity);
  config.local_peer_id = std::move(*peer_id);
  config.link_config = DefaultLedgerLinkConfig();
  config.udp_port = udp_port;
  return config;
}

pp::Roe<std::string> ParseBeaconMultiaddrString(const std::string& value) {
  if (value.empty()) {
    return pp::Error("beacon multiaddr cannot be empty");
  }
  if (value.front() == '/') {
    auto parsed = pp::amp::ParseAdpMultiaddr(value);
    if (!parsed) {
      return parsed.error();
    }
    return value;
  }
  return pp::Error("beacon entry must be an ADP multiaddr string");
}

} // namespace network
} // namespace pp
