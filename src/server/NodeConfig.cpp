#include "server/NodeConfig.h"
#include "ledger/network/amp/AmpIdentity.h"

#include <algorithm>

namespace pp {

using pp::common::Array;
using pp::common::Object;
using pp::common::Value;
using pp::common::asString;

pp::Roe<void> NodeConfig::fromJson(const Object &jd) {
  if (jd.contains("port")) {
    auto value = jd.getNonNegInt("port");
    if (!value || *value == 0 || *value > 65535) {
      return pp::Error("Field 'port' must be between 1 and 65535");
    }
    port = static_cast<uint16_t>(*value);
  }
  auto allowed = parseAllowedPeers(jd);
  if (!allowed) {
    return allowed.error();
  }
  allowedPeers = std::move(allowed.value());
  if (jd.contains("maxLinks")) {
    auto value = jd.getNonNegInt("maxLinks");
    if (!value || *value == 0 || *value > 65535) {
      return pp::Error("Field 'maxLinks' must be between 1 and 65535");
    }
    maxLinks = static_cast<uint32_t>(*value);
  }
  if (jd.contains("handshakeBacklog")) {
    auto value = jd.getNonNegInt("handshakeBacklog");
    if (!value || *value == 0 || *value > 65535) {
      return pp::Error("Field 'handshakeBacklog' must be between 1 and 65535");
    }
    handshakeBacklog = static_cast<uint32_t>(*value);
  }
  if (jd.contains("udpBufferBytes")) {
    auto value = jd.getNonNegInt("udpBufferBytes");
    if (!value || *value > kMaxUdpBufferBytes) {
      return pp::Error("Field 'udpBufferBytes' must be between 0 and " + std::to_string(kMaxUdpBufferBytes));
    }
    udpBufferBytes = static_cast<uint32_t>(*value);
  }
  if (handshakeBacklog > maxLinks) {
    return pp::Error("Field 'handshakeBacklog' may not exceed 'maxLinks'");
  }
  return {};
}

void NodeConfig::toJson(Object &j) const {
  j.setJsonUInt("port", port);
  if (allowedPeers) {
    std::vector<Value> peers(allowedPeers->begin(), allowedPeers->end());
    j.set("allowedPeers", Object::array(std::move(peers)));
  }
  if (maxLinks != kDefaultMaxLinks) {
    j.setJsonUInt("maxLinks", maxLinks);
  }
  if (handshakeBacklog != kDefaultHandshakeBacklog) {
    j.setJsonUInt("handshakeBacklog", handshakeBacklog);
  }
  if (udpBufferBytes != kDefaultUdpBufferBytes) {
    j.setJsonUInt("udpBufferBytes", udpBufferBytes);
  }
}

pp::Roe<std::optional<std::set<std::string>>> NodeConfig::parseAllowedPeers(const Object &jd) {
  if (!jd.contains("allowedPeers")) {
    return std::optional<std::set<std::string>>{};
  }
  const Array *list = jd.getArray("allowedPeers");
  if (!list) {
    return pp::Error("Field 'allowedPeers' must be an array of peer ids");
  }
  std::set<std::string> peers;
  for (const auto &value : list->elements) {
    auto id = asString(value);
    if (!id || id->empty()) {
      return pp::Error("Field 'allowedPeers' entries must be peer id strings");
    }
    peers.insert(*id);
  }
  return std::optional<std::set<std::string>>(std::move(peers));
}

pp::Roe<void> FollowerConfig::fromJson(const Object &jd) {
  if (auto common = NodeConfig::fromJson(jd); !common) {
    return common;
  }
  upstreams.clear();
  auto add = [this](const std::string &value, const std::string &field) -> pp::Roe<void> {
    auto multiaddr = network::ParseBeaconMultiaddrString(value);
    if (!multiaddr) {
      return pp::Error("Field '" + field + "': " + multiaddr.error().message);
    }
    if (std::find(upstreams.begin(), upstreams.end(), *multiaddr) == upstreams.end()) {
      upstreams.push_back(std::move(*multiaddr));
    }
    return {};
  };
  if (jd.contains("beacon")) {
    auto value = jd.getString("beacon");
    if (!value) {
      return pp::Error("Field 'beacon' must be an upstream multiaddr string");
    }
    if (auto added = add(*value, "beacon"); !added) {
      return added;
    }
  }
  if (jd.contains("beacons")) {
    const Array *list = jd.getArray("beacons");
    if (!list) {
      return pp::Error("Field 'beacons' must be an array of upstream multiaddrs");
    }
    for (size_t i = 0; i < list->elements.size(); ++i) {
      const std::string field = "beacons[" + std::to_string(i) + "]";
      auto value = asString(list->elements[i]);
      if (!value) {
        return pp::Error(field + " must be an upstream multiaddr string");
      }
      if (auto added = add(*value, field); !added) {
        return added;
      }
    }
  }
  if (upstreams.empty()) {
    return pp::Error("Field 'beacon' (or 'beacons'), the upstream multiaddr, is required");
  }
  anchor = {};
  if (const Object *anchorJson = jd.getObject("networkAnchor")) {
    auto parsed = NetworkAnchor::fromJson(*anchorJson);
    if (!parsed) {
      return parsed.error();
    }
    anchor = parsed.value();
  }
  return {};
}

void FollowerConfig::toJson(Object &j, const std::string &placeholder) const {
  NodeConfig::toJson(j);
  std::vector<Value> values(upstreams.begin(), upstreams.end());
  if (values.empty()) {
    values.push_back(placeholder);
  }
  j.set("beacons", Object::array(std::move(values)));
}

std::vector<env_config::Field> FollowerConfig::envFields() {
  std::vector<env_config::Field> fields{{"PP_LEDGER_UPSTREAMS", "beacons", env_config::Kind::List, "", false}};
  for (auto &field : env_config::anchorFields()) {
    fields.push_back(std::move(field));
  }
  return fields;
}

} // namespace pp
