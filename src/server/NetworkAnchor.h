#pragma once

#include "../client/Client.h"
#include "common/ResultOrError.hpp"
#include "lib/common/Meta.h"

#include <cstdint>
#include <string>

namespace pp {

/** Optional participant pin for network identity (see docs/ledger-topology.md §8). */
struct NetworkAnchor {
  std::string network_id;
  std::string genesis_hash;
  uint64_t trusted_checkpoint_id{0};
  bool trusted_checkpoint_set{false};

  bool isConfigured() const {
    return !network_id.empty() || !genesis_hash.empty() || trusted_checkpoint_set;
  }

  static NetworkAnchor fromJson(const pp::common::Object& jd) {
    NetworkAnchor anchor;
    if (auto id = jd.getString("networkId")) {
      anchor.network_id = *id;
    }
    if (auto hash = jd.getString("genesisHash")) {
      anchor.genesis_hash = *hash;
    }
    if (auto cp = jd.getNonNegInt("trustedCheckpointId")) {
      anchor.trusted_checkpoint_id = *cp;
      anchor.trusted_checkpoint_set = true;
    }
    return anchor;
  }

  template <typename ErrorType>
  static ResultOrError<void, ErrorType> verifyStatus(const NetworkAnchor& anchor,
                                                     const Client::BeaconState& state) {
    if (!anchor.network_id.empty() && state.networkId != anchor.network_id) {
      return ErrorType{-1, "upstream network id mismatch (expected " + anchor.network_id +
                                ", got " + state.networkId + ")"};
    }
    if (anchor.trusted_checkpoint_set && state.checkpointId < anchor.trusted_checkpoint_id) {
      return ErrorType{-1, "upstream checkpoint " + std::to_string(state.checkpointId) +
                                " is below trusted minimum " +
                                std::to_string(anchor.trusted_checkpoint_id)};
    }
    return {};
  }

  template <typename ErrorType>
  static ResultOrError<void, ErrorType> verifyGenesisHash(const NetworkAnchor& anchor,
                                                          const std::string& observed_hash) {
    if (anchor.genesis_hash.empty()) {
      return {};
    }
    if (observed_hash != anchor.genesis_hash) {
      return ErrorType{-1, "genesis hash mismatch"};
    }
    return {};
  }
};

} // namespace pp
