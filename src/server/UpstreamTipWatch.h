#ifndef PP_LEDGER_UPSTREAM_TIP_WATCH_H
#define PP_LEDGER_UPSTREAM_TIP_WATCH_H

#include "../client/Client.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>

namespace pp {

/**
 * Keeps one BLOCK_WAIT open to the upstream so new blocks propagate down the
 * tree as soon as the upstream has them: when the reply shows the upstream
 * ahead of us, `onUpstreamAhead` asks for a sync (BlockSync fetches the
 * blocks). The next wait starts from what the upstream already reported, so a
 * sync in progress does not turn it into polling. Failures back off; periodic sync stays the safety net, e.g. with an
 * upstream that does not serve BLOCK_WAIT. Server thread only, like BlockSync.
 */
class UpstreamTipWatch : public Module {
public:
  struct Hooks {
    /** Point the client at the upstream to watch. */
    std::function<pp::Roe<void>()> dialUpstream;
    /** Local next block id (tip + 1). */
    std::function<uint64_t()> nextBlockId;
    /** The upstream has blocks we lack: request a sync. */
    std::function<void()> onUpstreamAhead;
    /** Run a task on the server thread (Server::postToServerThread). */
    std::function<void(std::function<void()>)> postToServerThread;
  };

  struct Options {
    std::chrono::milliseconds initialBackoff{1000};
    std::chrono::milliseconds maxBackoff{30000};
  };

  UpstreamTipWatch(Client &client, Hooks hooks);
  UpstreamTipWatch(Client &client, Hooks hooks, Options options);

  /** Run loop: arm a wait unless one is out or a failure is backing off. */
  void maintain();
  bool inFlight() const { return inFlight_; }

private:
  void onReply(Client::Roe<uint64_t> result);

  Client &client_;
  Hooks hooks_;
  Options options_;
  /** Lets late network callbacks detect that this object is gone. */
  std::shared_ptr<int> alive_{std::make_shared<int>(0)};
  bool inFlight_{false};
  /** Upstream's next block id from its last reply: wait only for blocks beyond it (a sync may be running). */
  uint64_t upstreamNext_{0};
  std::chrono::milliseconds backoff_{0};
  std::chrono::steady_clock::time_point notBefore_{};
};

} // namespace pp

#endif // PP_LEDGER_UPSTREAM_TIP_WATCH_H
