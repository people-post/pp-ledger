#ifndef PP_LEDGER_UPSTREAM_SET_H
#define PP_LEDGER_UPSTREAM_SET_H

#include "../client/Client.h"
#include "common/Module.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace pp {

/**
 * The upstreams a relay or miner may follow, and the one it follows (beacon-authority bp2b): the
 * configured ones, then the gateways a beacon certificate hints at (`dialHints`). It re-ranks them —
 * STATUS from all of them in parallel, then the highest beacon generation this node would follow,
 * then the longest chain — every kRerankInterval, on a new certificate and when a sync fails (the
 * failed one is avoided for kAvoidFailedFor). A relay whose beacon stood down keeps syncing without
 * a failure, so only ranking moves it to the new beacon. It also pushes certificates to every
 * upstream, so one learned downstream reaches a relay that sits behind the old beacon.
 *
 * Network calls go through hooks, results arrive on the server thread. Server thread only.
 */
class UpstreamSet : public Module {
public:
  using StatusDone = std::function<void(Client::Roe<Client::BeaconState>)>;

  struct Hooks {
    /** Fetch STATUS from upstream `index` at `multiaddr`; `done` runs on the server thread. */
    std::function<void(size_t index, const std::string &multiaddr, StatusDone done)> fetchStatus;
    /** Optional: whether an upstream's STATUS is for this node's network (false: disregard it, logged). */
    std::function<bool(size_t index, const Client::BeaconState &state)> admit;
    /**
     * Take what an admitted STATUS carries — its certificate, its equivocation evidence — before
     * ranking: a certificate one upstream serves may rule out another's tip.
     */
    std::function<void(size_t index, const Client::BeaconState &state)> learn;
    /** Two upstreams serve tips of one generation and height for different blocks (ba6). */
    std::function<void(const Client::Equivocation &evidence, const std::string &from)> offerEquivocation;
    /** Whether an upstream serving this tip is worth following (Chain::mayFollowTip). */
    std::function<bool(const Client::SignedTip &tip)> mayFollow;
    /** A hint becomes an upstream if this accepts it (Server::addUpstream: not this node itself). */
    std::function<bool(const std::string &multiaddr)> adopt;
    /** The upstream followed changed (Server::followUpstream). */
    std::function<void(const std::string &multiaddr)> onFollow;
  };

  static constexpr std::chrono::seconds kRerankInterval{60};
  static constexpr std::chrono::seconds kAvoidFailedFor{60};
  static constexpr std::chrono::seconds kSuspectGap{5};
  static constexpr size_t kMaxUpstreams = 32;
  /** Within one generation, the upstream followed is kept unless this many blocks behind (no flapping). */
  static constexpr uint64_t kLagTolerance = 2;

  UpstreamSet(std::vector<std::string> configured, Hooks hooks);

  /** The AMP dial key of upstream `index`: one per upstream, so calls to several can run at once. */
  static std::string dialKey(size_t index) { return "upstream:" + std::to_string(index); }

  const std::vector<std::string> &all() const { return upstreams_; }
  size_t activeIndex() const { return active_; }
  /** The upstream followed now; empty when there is none. */
  const std::string &active() const;

  /** Add a certificate's dial hints not present yet (valid ADP multiaddrs only); how many were added. */
  size_t addHints(const std::vector<std::string> &hints);

  /**
   * STATUS from every upstream, then follow the best (choose). One round at a time: asked again
   * meanwhile (a hook may), another round follows this one.
   */
  void rerank(const std::string &why);
  /** A round is in flight. */
  bool ranking() const { return inFlight_; }
  /** The last round's choice (none before one, or when no upstream qualified). */
  const std::optional<size_t> &chosen() const { return chosen_; }
  /** The STATUS upstream `index` gave in the last round (none where it failed or was not admitted). */
  const std::optional<Client::BeaconState> &lastStatus(size_t index) const;
  /** The active upstream's sync failed: avoid it a while, move on now, and re-rank. */
  void noteFailed();
  /**
   * The upstream followed refused what it should take (a block: it may be a beacon that stood
   * down, or behind the new one): re-rank now, unless a round ran within kSuspectGap.
   */
  void suspect(const std::string &why);
  /** Run loop: re-rank every kRerankInterval when there is a choice. */
  void maintain();
  /**
   * The upstream to follow among `states` (none where STATUS failed): skip those in `avoid` and
   * those whose tip `mayFollow` rules out; then the highest tip generation, then the highest next
   * block id — but `active` stays while of that generation and at most kLagTolerance blocks behind.
   * None when no upstream qualifies.
   */
  static std::optional<size_t> choose(const std::vector<std::optional<Client::BeaconState>> &states,
                                      const std::function<bool(const Client::SignedTip &)> &mayFollow,
                                      const std::set<size_t> &avoid, size_t active);

private:
  using Clock = std::chrono::steady_clock;

  void onStatus(uint64_t round, size_t index, Client::Roe<Client::BeaconState> status);
  void finishRound();
  /** Follow upstream `index` from now (logged with `why`). */
  void follow(size_t index, const std::string &why);
  /** Pairs of tips in `states_` that equivocate: offered (offerEquivocation). */
  void offerConflictingTips();
  std::set<size_t> avoided() const;

  std::vector<std::string> upstreams_;
  Hooks hooks_;
  size_t active_{0};
  /** Upstreams whose sync failed, and until when they are avoided. */
  std::map<size_t, Clock::time_point> avoidUntil_;

  uint64_t round_{0};
  bool inFlight_{false};
  bool again_{false};
  std::string why_;
  size_t pending_{0};
  std::vector<std::optional<Client::BeaconState>> states_;
  std::optional<size_t> chosen_;
  Clock::time_point lastRanked_{};
};

} // namespace pp

#endif // PP_LEDGER_UPSTREAM_SET_H
