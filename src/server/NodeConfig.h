#ifndef PP_LEDGER_NODE_CONFIG_H
#define PP_LEDGER_NODE_CONFIG_H

#include "EnvConfig.h"
#include "NetworkAnchor.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Meta.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

/** config.json fields every node reads (CONFIGURATION.md § config.json): `port`, `allowedPeers`. */
struct NodeConfig {
  uint16_t port{0};
  /** `allowedPeers`: serve only these peer ids (and upstreams); absent = anyone. */
  std::optional<std::set<std::string>> allowedPeers;

  explicit NodeConfig(uint16_t defaultPort = 0) : port(defaultPort) {}

  /** What `jd` sets; without `port`, the default stays. */
  pp::Roe<void> fromJson(const pp::common::Object &jd);
  void toJson(pp::common::Object &j) const;

  /** `allowedPeers`, an array of peer ids; absent = nullopt. */
  static pp::Roe<std::optional<std::set<std::string>>> parseAllowedPeers(const pp::common::Object &jd);
};

/** A follower's (relay, miner): the upstreams it may follow, and the network it expects. */
struct FollowerConfig : NodeConfig {
  /** `beacon` and/or `beacons` (bp2b): upstream multiaddrs, in order, without repeats; at least one. */
  std::vector<std::string> upstreams;
  /** `networkAnchor` (LEDGER_TOPOLOGY.md § Network anchor). */
  NetworkAnchor anchor;

  using NodeConfig::NodeConfig;

  pp::Roe<void> fromJson(const pp::common::Object &jd);
  /** `placeholder` stands in `beacons` while there is no upstream (a template to fill in). */
  void toJson(pp::common::Object &j, const std::string &placeholder) const;

  /** The variables a follower reads besides env_config::commonFields: upstreams and the anchor. */
  static std::vector<env_config::Field> envFields();
};

} // namespace pp

#endif // PP_LEDGER_NODE_CONFIG_H
