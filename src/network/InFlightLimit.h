#ifndef PP_LEDGER_IN_FLIGHT_LIMIT_H
#define PP_LEDGER_IN_FLIGHT_LIMIT_H

#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <string>
#include <unordered_map>

namespace pp::network {

/**
 * Requests this node has in flight to each peer (dial key), at most `limit` — what the peer takes
 * at once (ledger::rpc::kMaxReceivingPerPeer) — the rest waiting here (load-resilience lr6, LR007),
 * so a client never meets the peer's refusal of one more channel. io lane only.
 *
 * Two classes (lr8, LR009): a node's own chain work (Duty: blocks, pulls, sync) and everything else
 * (Normal: what it forwards for its clients, a client's own requests). Normal requests leave
 * `reserved` places free for duties, and duties waiting start first, so a flood of forwarded
 * requests cannot hold up a block. At most `maxWaiting` normal requests wait per peer; one more is
 * refused at once — the caller answers "busy" instead of letting it wait out its deadline.
 */
class InFlightLimit {
public:
  enum class Class { Duty, Normal };

  explicit InFlightLimit(size_t limit, size_t reserved = 0,
                         size_t maxWaiting = std::numeric_limits<size_t>::max())
      : limit_(limit), reserved_(reserved < limit ? reserved : 0), maxWaiting_(maxWaiting) {}

  struct Waiter {
    std::function<bool()> live;  ///< false once it gave up waiting (skipped)
    std::function<void()> start; ///< starts the request; it calls release() when it ends
  };

  /** Start `waiter` now if `peer` has room for its class, else queue it; false (dropped) when a
   *  normal one finds `maxWaiting` ahead of it. */
  bool run(const std::string &peer, Waiter waiter, Class cls = Class::Normal);
  /** A started request to `peer` ended: start the next live waiter that has room. */
  void release(const std::string &peer);

  size_t inFlight(const std::string &peer) const;
  size_t waiting(const std::string &peer) const;

private:
  struct Slot {
    size_t inFlight{0};
    std::deque<Waiter> duties;
    std::deque<Waiter> normal;
  };

  /** Room for one more of `cls` with `inFlight` started. */
  bool hasRoom(size_t inFlight, Class cls) const {
    return inFlight < (cls == Class::Duty ? limit_ : limit_ - reserved_);
  }
  /** Start live waiters while there is room: duties first. */
  void startWaiting(const std::string &peer);

  size_t limit_;
  size_t reserved_;
  size_t maxWaiting_;
  std::unordered_map<std::string, Slot> slots_;
};

} // namespace pp::network

#endif // PP_LEDGER_IN_FLIGHT_LIMIT_H
