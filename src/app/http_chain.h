#ifndef PP_LEDGER_HTTP_CHAIN_H
#define PP_LEDGER_HTTP_CHAIN_H

// pp-http's /api/chain/: what a service following the chain reads from its own relay — the node's
// state, its blocks as transfers, an account's balances — with amounts and ids as decimal strings.
// Stateless: each answer is the node's (docs/contracts/SERVICE_API.md).

#include "ledger/client/Client.h"

#include <httplib.h>

/** Register /api/chain/ on `svr`, answered by the node `node` reads. */
void registerChainRoutes(httplib::Server &svr, pp::Client &node);

#endif
