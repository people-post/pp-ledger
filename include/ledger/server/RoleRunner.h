#ifndef PP_LEDGER_ROLE_RUNNER_H
#define PP_LEDGER_ROLE_RUNNER_H

#include "ledger/server/Server.h"
#include "ledger/network/HostedAmpRuntime.h"
#include "common/Module.h"
#include "ledger/lib/common/Service.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace pp {

/**
 * Runs one ledger role on its work directory until asked to stop (platform-integration pi2): what
 * each app's main() did. A rewind is a restart (beacon-authority B022), so it runs a fresh server
 * on the same directory for as long as the last one asks to be restarted.
 *
 * The apps run it on their main thread; a host — pp-node — on a thread of its own, on its Amp
 * runtime (Options::host). It installs no signal handler and sets no log level: those are the
 * process's.
 */
class RoleRunner : public Module {
public:
  enum class Role { Beacon, Relay, Miner };

  struct Options {
    Role role{Role::Relay};
    std::string workDir;
    /** --approve-rewind: the generation whose deep rewind the operator approved (0: none). */
    uint64_t approvedRewindGeneration{0};
    /** The log file: empty = the role's default in the work directory, "none" = no file. */
    std::string logFile;
    /** Serve and call on a host's Amp runtime (Server::runOnHost); none: a socket of its own. */
    std::optional<network::HostedAmpRuntime::Host> host;
    /**
     * On a host: told when the role's ledger protocols are bound (true) and about to be unbound
     * (false), once per server — a restart (a rewind) unbinds and binds again. The host advertises
     * AmpLedgerServer::protocolIds() while they are bound (Server::setOnListening). The role's thread.
     */
    std::function<void(bool listening)> onListening;
    /** Beacon, first start only: --promote, --accept-ledger-tip. */
    bool promote{false};
    bool acceptLedgerTip{false};
    /** Tests: the server to run in place of the role's (each start makes a fresh one). */
    std::function<std::unique_ptr<Server>()> makeServer;
  };

  /** "beacon", "relay", "miner"; none for anything else. */
  static std::optional<Role> roleNamed(const std::string &name);
  static std::string nameOf(Role role);

  explicit RoleRunner(Options options);

  /**
   * Blocks: runs servers until stop(), or until one ends without asking for a restart. An error
   * when a server fails to run (its start refused: config, keys, a failed startup sync…), or for
   * a beacon on a host (a beacon runs standalone only).
   */
  Service::Roe<void> run();
  /**
   * Any thread, and a signal handler (one atomic store): the server running stops, and no other
   * starts — also when it comes before run().
   */
  void stop() { stop_ = true; }
  bool stopRequested() const { return stop_; }

private:
  std::unique_ptr<Server> makeServer(bool firstStart) const;

  Options options_;
  std::atomic<bool> stop_{false};
};

} // namespace pp

#endif // PP_LEDGER_ROLE_RUNNER_H
