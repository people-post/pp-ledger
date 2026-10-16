#ifndef PP_LEDGER_BLOCK_SYNC_H
#define PP_LEDGER_BLOCK_SYNC_H

#include "../chain/AuthorityTrail.h"
#include "../client/Client.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pp {

/**
 * Pulls missing blocks from an upstream without blocking the server thread, and commits only
 * blocks a beacon signed tip vouches for (beacon-authority ba4).
 *
 * 1. STATUS: the upstream's signed tip.
 * 2. Authenticate: fetch the blocks from the local tip to the signed tip and check that each one
 *    hashes to what it claims and links to the one before, from the local tip up to the signed
 *    block hash; follow the beacon authority along them (AuthorityTrail: admin-signed generation
 *    and admin-set changes) and check the tip against the generation in force at its height.
 *    Nothing is committed yet. Up to `maxHeldBlocks` bodies are kept; beyond that only their
 *    hashes.
 * 3. Apply: commit the blocks in order (fetching again those not kept), each matching the hash
 *    authenticated for its height.
 *
 * A node with no blocks yet authenticates its range from its first block — genesis, or a
 * checkpoint opening with the genesis renewal — which is where it learns the beacon's key and the
 * admin set (AuthorityTrail::fromStartingBlock); nothing commits before the tip verifies under
 * them. That first block is the node's root of trust: `firstBlockHash` pins it (an anchored
 * genesis), `checkStartingAuthority` vets the authority it carries (an anchored miner's proof);
 * without either it is taken on trust.
 *
 * A few requests are in flight at a time; one sync at a time; failures back off exponentially.
 * Network results reach this object only through `postToServerThread`, so every member function
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
    /** Hash of the local tip; none while the node holds no block. */
    std::function<std::optional<std::string>()> tipHash;
    /** Optional: the hash the first block must have when the node holds none. */
    std::function<std::optional<std::string>()> firstBlockHash;
    /** Optional: vet the authority and admin set a node with no blocks takes from block `blockId`. */
    std::function<pp::Roe<void>(uint64_t blockId, const ChainAuthority &, const Client::Wallet &admin)>
        checkStartingAuthority;
    /** The beacon authority at the local tip, to follow along the blocks being synced. */
    std::function<pp::Roe<AuthorityTrail>()> authorityTrail;
    /** Optional: a signed tip this sync verified (the node may serve it on). */
    std::function<void(const Client::SignedTip &)> onTipVerified;
    /** Validate and append one block (its id is nextBlockId()). */
    std::function<pp::Roe<void>(const Ledger::ChainNode &)> applyBlock;
    /** Run a task on the server thread (Server::postToServerThread). */
    std::function<void(std::function<void()>)> postToServerThread;
    /** Called once per sync, on the server thread. */
    std::function<void(const Result &)> onFinished;
  };

  struct Options {
    size_t maxInFlightFetches{4};
    /** Authenticated block bodies kept for the apply pass; beyond this they are fetched again. */
    size_t maxHeldBlocks{1024};
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
  enum class Phase {
    Status,       ///< waiting for the upstream's STATUS
    Authenticate, ///< hash chain from the local tip (or the first block) to the signed tip; nothing committed
    Apply,        ///< commit the authenticated blocks in order
  };

  void onStatus(Client::Roe<Client::BeaconState> result);
  /** Check the upstream's signed tip and start authenticating up to it. */
  void syncToSignedTip();
  void beginRange(Phase phase, uint64_t from, uint64_t to);
  void onBlock(uint64_t blockId, Client::Roe<Ledger::ChainNode> result);
  void fetchMore();
  /** Process ready blocks in order; ends the phase at the range's end. */
  void advance();
  /** One block of the current phase; false if the sync finished (failed). */
  bool process(Ledger::ChainNode block);
  bool authenticate(Ledger::ChainNode block);
  /** A node with no blocks: its first block starts the trail (and may be pinned or vetted). */
  bool authenticateFirst(Ledger::ChainNode &block);
  bool apply(Ledger::ChainNode block);
  /** The current phase covered its range: start the next, or finish. */
  void endPhase();
  /** Check tip_ against trail_; finish (failed) if it does not verify. */
  bool verifyTip();
  void finish(bool ok, std::string error);

  /** Wrap a client callback: hop to the server thread, drop if we are gone or stale. */
  template <typename T, typename Fn> Client::Done<T> onServerThread(Fn fn);

  Client &client_;
  Hooks hooks_;
  Options options_;
  /** Lets late network callbacks detect that this object is gone. */
  std::shared_ptr<int> alive_{std::make_shared<int>(0)};

  bool inFlight_{false};
  /** Counts syncs, so results of a finished one are dropped (not a beacon generation). */
  uint64_t attempt_{0};
  Phase phase_{Phase::Status};
  Client::BeaconState status_;
  Client::SignedTip tip_;
  /** Authority along the blocks authenticated so far (from the local tip). */
  std::optional<AuthorityTrail> trail_;
  /** Current phase's range [base_, target_). */
  uint64_t base_{0};
  uint64_t target_{0};
  uint64_t nextToFetch_{0};
  uint64_t nextToProcess_{0};
  size_t outstanding_{0};
  uint64_t blocksAdded_{0};
  /** Arrived out of order, waiting for their turn. */
  std::map<uint64_t, Ledger::ChainNode> fetched_;
  /** Authenticate: hash of the block before the next one. */
  std::string linkHash_;
  /** The node holds no block: the range starts its chain. */
  bool freshStart_{false};
  /** Authenticated hashes from `authBase_` up to the signed tip. */
  uint64_t authBase_{0};
  std::vector<std::string> authHashes_;
  /** Authenticated bodies kept for Apply (at most maxHeldBlocks). */
  std::map<uint64_t, Ledger::ChainNode> held_;
  std::chrono::milliseconds backoff_{0};
  std::chrono::steady_clock::time_point notBefore_{};
};

} // namespace pp

#endif // PP_LEDGER_BLOCK_SYNC_H
