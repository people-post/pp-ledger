#ifndef PP_LEDGER_REWIND_H
#define PP_LEDGER_REWIND_H

#include "../client/Client.h"
#include "common/Error.h"

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
