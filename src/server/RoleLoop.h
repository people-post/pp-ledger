#ifndef PP_LEDGER_ROLE_LOOP_H
#define PP_LEDGER_ROLE_LOOP_H

#include "ledger/server/RoleRunner.h"
#include "server/Server.h"

#include "common/Logger.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace pp {

/** A fresh server for the role; `firstStart` is false after a restart (a rewind). */
using RoleServerFactory = std::function<std::unique_ptr<Server>(bool firstStart)>;

/**
 * The loop a role runs in (beacon-authority B022: a rewind is a restart): servers from `make`, each
 * set up from `options` and run on its work directory, a fresh one after each restart, until `stop`
 * or a server ends without asking for one. `stop` is every server's stop source, so a stop between
 * two servers holds. RoleRunner runs it for its role; tests for a server of their own.
 */
Service::Roe<void> runRoleLoop(const RoleRunner::Options &options, const std::string &role,
                               const RoleServerFactory &make, const std::atomic<bool> &stop,
                               logging::Logger &log);

} // namespace pp

#endif // PP_LEDGER_ROLE_LOOP_H
