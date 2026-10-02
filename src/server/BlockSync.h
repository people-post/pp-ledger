#ifndef PP_LEDGER_BLOCK_SYNC_H
#define PP_LEDGER_BLOCK_SYNC_H

#include "../client/Client.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>

namespace pp {

/**
 * Pulls missing blocks from an upstream without blocking the server thread.
 *
 * Calibrate (learn the upstream's next block id), then fetch the gap with a
 * few requests in flight, applying blocks strictly in order as they arrive.
 * One sync at a time; failures back off exponentially. Network results reach
 * this object only through `postToServerThread`, so every member function
 * runs on the server thread. See docs/architecture/THREADING.md.
 */
class BlockSync : public Module {
public:
  struct Result {
    bool ok{false};
    uint64_t blocksAdded{0};
    std::string error;
  };

  struct Hooks {
    /** Point the client at the upstream to sync from. */
    std::function<pp::Roe<void>()> dialUpstream;
    /** Local next block id (tip + 1). */
    std::function<uint64_t()> nextBlockId;
    /** Validate and append one block (its id is nextBlockId()). */
    std::function<pp::Roe<void>(const Ledger::ChainNode &)> applyBlock;
    /** Run a task on the server thread (Server::postToServerThread). */
    std::function<void(std::function<void()>)> postToServerThread;
    /** Called once per sync, on the server thread. */
    std::function<void(const Result &)> onFinished;
  };

  struct Options {
    size_t maxInFlightFetches{4};
    std::chrono::milliseconds initialBackoff{500};
    std::chrono::milliseconds maxBackoff{8000};
  };

  enum class Start { Started, AlreadyRunning, BackingOff };

  BlockSync(Client &client, Hooks hooks);
  BlockSync(Client &client, Hooks hooks, Options options);

  /** Begin a sync unless one is running or a recent failure is backing off. */
  Start start();
  bool inFlight() const { return inFlight_; }

private:
  void onCalibrated(uint64_t generation, Client::Roe<Client::CalibrationResponse> result);
  void onBlock(uint64_t generation, uint64_t blockId, Client::Roe<Ledger::ChainNode> result);
  void fetchMore();
  /** Apply fetched blocks that extend the tip; false if one failed (sync finished). */
  bool applyReady();
  void finish(bool ok, std::string error);

  /** Wrap a client callback: hop to the server thread, drop if we are gone or stale. */
  template <typename T, typename Fn> Client::Done<T> onServerThread(Fn fn);

  Client &client_;
  Hooks hooks_;
  Options options_;
  /** Lets late network callbacks detect that this object is gone. */
  std::shared_ptr<int> alive_{std::make_shared<int>(0)};

  bool inFlight_{false};
  uint64_t generation_{0};
  uint64_t target_{0};
  uint64_t nextToFetch_{0};
  size_t outstanding_{0};
  uint64_t blocksAdded_{0};
  std::map<uint64_t, Ledger::ChainNode> fetched_;
  std::chrono::milliseconds backoff_{0};
  std::chrono::steady_clock::time_point notBefore_{};
};

} // namespace pp

#endif // PP_LEDGER_BLOCK_SYNC_H
