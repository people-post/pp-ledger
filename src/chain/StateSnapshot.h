#ifndef PP_LEDGER_STATE_SNAPSHOT_H
#define PP_LEDGER_STATE_SNAPSHOT_H

#include "Types.h"
#include "../client/Client.h"
#include "common/ResultOrError.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pp {

/**
 * A network's account state at one height, for a re-genesis to import (beacon-authority B030,
 * B033): the predecessor it came from, the config in force there, and every account with the
 * fields its state-root leaf was hashed from plus its attachment. `accountsRoot()` must equal
 * `predecessor.stateRoot` — block `height`'s — which is what a new network checks the import
 * against (bp9).
 */
struct StateSnapshot {
  constexpr static const uint32_t VERSION = 1;

  /** Where the state comes from: the old network and its block at `height`. */
  struct Predecessor {
    std::string networkId;
    uint64_t height{0};
    std::string blockHash; ///< raw 32 bytes
    std::string stateRoot; ///< raw 32 bytes: block `height`'s header

    bool operator==(const Predecessor &other) const = default;
    template <typename Archive> void serialize(Archive &ar) { ar & networkId & height & blockHash & stateRoot; }
  };

  /** One account as the old chain held it: its leaf fields, and its attachment's bytes. */
  struct Account {
    uint64_t id{0};
    Client::Wallet wallet;
    uint64_t blockId{0}; ///< the old block of its last write (the leaf hashes it)
    std::string attachment;

    template <typename Archive> void serialize(Archive &ar) { ar & id & wallet & blockId & attachment; }
  };

  Predecessor predecessor;
  /** The old network's config in force at `height` (a new genesis carries it, B032). */
  BlockChainConfig config;
  /** By ascending id. */
  std::vector<Account> accounts;

  template <typename Archive> void serialize(Archive &ar) { ar & predecessor & config & accounts; }

  std::string ltsToString() const;
  bool ltsFromString(const std::string &str);

  /** Root of the accounts' state-tree leaves (account leaf v2), as the old chain computed it. */
  std::string accountsRoot() const;
};

namespace state_export {

struct Error : RoeErrorBase {
  using RoeErrorBase::RoeErrorBase;
};
template <typename T> using Roe = ResultOrError<T, Error>;

/**
 * The state of the ledger in `ledgerDir` — a full node's (a relay's: from genesis) — after block
 * `height`. Leaves `ledgerDir` as it is: copies it into `scratchDir` (which must not exist: created,
 * then removed whole), cuts
 * the copy after `height`, replays it from genesis, and dumps every account. Checks the dump's
 * root against block `height`'s state root before returning it.
 */
Roe<StateSnapshot> exportState(const std::string &ledgerDir, uint64_t height, const std::string &scratchDir);

} // namespace state_export

} // namespace pp

#endif // PP_LEDGER_STATE_SNAPSHOT_H
