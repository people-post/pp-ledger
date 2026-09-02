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

bool ParseIpv4Host(const std::string& host, std::array<int, 4>& octets) {
  std::istringstream stream(host);
  char dot = 0;
  for (int i = 0; i < 4; ++i) {
    if (!(stream >> octets[static_cast<size_t>(i)]) || octets[static_cast<size_t>(i)] < 0 ||
        octets[static_cast<size_t>(i)] > 255) {
      return false;
    }
    if (i < 3) {
      if (!(stream >> dot) || dot != '.') {
        return false;
      }
    }
  }
  std::string trailing;
  return !(stream >> trailing);
}

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

pp::Roe<pp::adp::IpEndpoint> HostPortToEndpoint(const std::string& host, uint16_t port) {
  if (host == "localhost") {
    return pp::adp::IpEndpoint::V4(127, 0, 0, 1, port);
  }
  std::array<int, 4> octets{};
  if (ParseIpv4Host(host, octets)) {
    return pp::adp::IpEndpoint::V4(static_cast<uint8_t>(octets[0]), static_cast<uint8_t>(octets[1]),
                                   static_cast<uint8_t>(octets[2]), static_cast<uint8_t>(octets[3]), port);
  }
  return pp::Error("unsupported beacon host (use IPv4 or localhost)");
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

pp::Roe<std::string> ParseBeaconMultiaddr(const pp::common::Object& entry) {
  if (auto s = entry.getString("multiaddr")) {
    return ParseBeaconMultiaddrString(*s);
  }
  if (entry.contains("host") || entry.contains("port") || entry.contains("peerId")) {
    auto host = entry.getString("host");
    auto port = entry.getNonNegInt("port");
    auto peer_id = entry.getString("peerId");
    if (!host || host->empty()) {
      return pp::Error("beacon host is required");
    }
    if (!port || *port == 0 || *port > 65535) {
      return pp::Error("beacon port must be 1-65535");
    }
    if (!peer_id || peer_id->empty()) {
      return pp::Error("beacon peerId is required for AMP");
    }
    auto endpoint = HostPortToEndpoint(*host, static_cast<uint16_t>(*port));
    if (!endpoint) {
      return endpoint.error();
    }
    auto ma = pp::amp::FormatAdpMultiaddr(*endpoint, *peer_id);
    if (!ma) {
      return ma.error();
    }
    return *ma;
  }
  return pp::Error("beacon entry must be a multiaddr string or {host, port, peerId}");
}

} // namespace network
} // namespace pp
