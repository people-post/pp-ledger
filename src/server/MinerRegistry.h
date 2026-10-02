#ifndef PP_LEDGER_MINER_REGISTRY_H
#define PP_LEDGER_MINER_REGISTRY_H

#include "../client/Client.h"

#include <cstdint>
#include <map>

namespace pp {

/**
 * The beacon's registry of miners. Miners renew their record periodically;
 * one not renewed within the TTL is dropped. `version` changes only when the
 * list does (a miner joins, changes endpoint, or expires), not on renewals.
 */
class MinerRegistry {
public:
  /** Record or renew `miner`, stamped `nowSec`. */
  void upsert(Client::MinerInfo miner, int64_t nowSec) {
    miner.tLastMessage = nowSec;
    auto it = miners_.find(miner.id);
    const bool changed = it == miners_.end() || it->second.endpoint != miner.endpoint;
    miners_[miner.id] = std::move(miner);
    if (changed) {
      ++version_;
    }
  }

  /** Drop records last renewed more than `ttlSec` before `nowSec`; returns how many. */
  size_t expire(int64_t nowSec, int64_t ttlSec) {
    const size_t removed =
        std::erase_if(miners_, [&](const auto &entry) { return nowSec - entry.second.tLastMessage > ttlSec; });
    if (removed > 0) {
      ++version_;
    }
    return removed;
  }

  const std::map<uint64_t, Client::MinerInfo> &miners() const { return miners_; }
  uint64_t version() const { return version_; }

private:
  std::map<uint64_t, Client::MinerInfo> miners_;
  uint64_t version_{0};
};

} // namespace pp

#endif // PP_LEDGER_MINER_REGISTRY_H
