#ifndef PP_LEDGER_PREDECESSOR_CHECK_H
#define PP_LEDGER_PREDECESSOR_CHECK_H

#include "ledger/ledger/Ledger.h"
#include "common/ResultOrError.hpp"

#include <string>

namespace pp::predecessor_check {

/**
 * A node that ran the old network vouches for a re-genesis itself (beacon-authority B037): `genesis`
 * — block 0 of the network it is about to follow — must be an importing genesis whose predecessor is
 * block H of the ledger in `oldWorkDir` (the old node's work directory; stop that node first): the
 * same network id, block hash and state root. Refused otherwise, and when that ledger does not hold
 * block H (it cannot vouch for it).
 */
pp::Roe<void> check(const Ledger::ChainNode &genesis, const std::string &oldWorkDir);

} // namespace pp::predecessor_check

#endif
