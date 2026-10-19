#ifndef PP_LEDGER_NODE_CONFIG_H
#define PP_LEDGER_NODE_CONFIG_H

#include "server/EnvConfig.h"
#include "server/NetworkAnchor.h"
#include "common/ResultOrError.hpp"
#include "ledger/lib/common/Meta.h"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

/**
 * config.json fields every node reads (CONFIGURATION.md § config.json): `port`, `allowedPeers`,
 * `maxLinks`, `handshakeBacklog`, `udpBufferBytes`.
 */
struct NodeConfig {
  /** Links a node holds at once, and handshakes in progress (load-resilience LR005; until lr8 sizes them). */
  static constexpr uint32_t kDefaultMaxLinks = 1024;
  static constexpr uint32_t kDefaultHandshakeBacklog = 128;
  /**
   * The socket's receive and send buffers asked for: one socket carries every peer, and the OS
   * default (Linux: 208 KiB) drops datagrams of all peers alike under a burst of large requests
   * (load-resilience M12). The OS grants at most its cap (`net.core.rmem_max` / `wmem_max`).
   */
  static constexpr uint32_t kDefaultUdpBufferBytes = 8 * 1024 * 1024;
  static constexpr uint32_t kMaxUdpBufferBytes = 1024 * 1024 * 1024;

  uint16_t port{0};
  /** `allowedPeers`: serve only these peer ids (and upstreams); absent = anyone. */
  std::optional<std::set<std::string>> allowedPeers;
  /** `maxLinks`: peers held at once (TCP: connections); full, the longest-idle inbound link makes room. */
  uint32_t maxLinks{kDefaultMaxLinks};
  /** `handshakeBacklog`: inbound handshakes in progress (TCP: the listen backlog); full, a hello waits for its resend. */
  uint32_t handshakeBacklog{kDefaultHandshakeBacklog};
  /** `udpBufferBytes`: the UDP socket's receive and send buffers; 0 = the OS default. */
  uint32_t udpBufferBytes{kDefaultUdpBufferBytes};

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
