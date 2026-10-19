#ifndef PP_LEDGER_REWIND_H
#define PP_LEDGER_REWIND_H

#include "ledger/chain/Chain.h"

#include "ledger/client/Client.h"
#include "common/Error.h"

#include <chrono>
#include <optional>
#include <string>

namespace pp::rewind {

/**
 * A rewind is a restart (beacon-authority bp1, B022). A node ordered to rewind — it holds blocks
 * above the cutover of a later certificate — writes the order to `rewind.dat` in its data
 * directory and restarts; the fresh server applies it before anything mounts the ledger, then
 * keeps it in `rewound.dat` as the last rewind (STATUS). Each step can be repeated after a crash.
 */
constexpr const char *FILE_ORDER = "rewind.dat";
constexpr const char *FILE_DONE = "rewound.dat";

/**
 * How far back a node rewinds on its own (beacon-authority bp1): seven days of slots. A deeper
 * cutover waits for its operator to approve its generation (`--approve-rewind`), so a mistaken
 * certificate cannot cut a network's history unattended.
 */
constexpr std::chrono::seconds kMaxAutomaticRewind{7 * 24 * 3600};

/** What schedule() did. */
struct Scheduled {
  enum class Kind {
    None,     ///< no rewind ordered
    Ordered,  ///< the order is written: the node restarts to apply it
    HeldBack, ///< deeper than the node rewinds on its own, and not approved
    Forked,   ///< the node's block at the cutover is not the one the certificate vouches for
  };
  Kind kind{Kind::None};
  std::optional<Client::RewindReport> order;
  /** Slot time the order reaches back; none when it could not be read (held back). */
  std::optional<uint64_t> depthSeconds;
};

/**
 * Write the rewind `chain`'s held certificate orders (Chain::rewindOrder) to `dataDir`, unless the
 * node holds another block at the cutover (Forked: never on its own — a fork at or below it, or a
 * mistaken certificate), or the rewind reaches back further than `maxAutomatic` (or that cannot be
 * read) and the operator has not approved its generation (`approvedGeneration`, 0 = none).
 */
pp::Roe<Scheduled> schedule(const Chain &chain, const std::string &dataDir, uint64_t approvedGeneration,
                            std::chrono::seconds maxAutomatic = kMaxAutomaticRewind);

/** Write `order` durably, for the next start to apply. */
pp::Roe<void> writeOrder(const std::string &dataDir, const Client::RewindReport &order);

/**
 * At start, before the ledger mounts: apply a pending order to the ledger at `ledgerDir` — keep the
 * blocks up to `keptUpTo`, or remove the ledger when that lies below its first block (the node then
 * joins fresh) — record it as the last rewind and drop the order. None when no order was pending.
 */
pp::Roe<std::optional<Client::RewindReport>> applyPendingOrder(const std::string &dataDir,
                                                               const std::string &ledgerDir);

/** The last rewind this node applied; none if none. */
pp::Roe<std::optional<Client::RewindReport>> lastApplied(const std::string &dataDir);

} // namespace pp::rewind

#endif // PP_LEDGER_REWIND_H
