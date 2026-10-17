#ifndef PP_LEDGER_IN_FLIGHT_LIMIT_H
#define PP_LEDGER_IN_FLIGHT_LIMIT_H

#include <cstddef>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>

namespace pp::network {

/**
 * Requests this node has in flight to each peer (dial key), at most `limit` — what the peer takes
 * at once (ledger::rpc::kMaxReceivingPerPeer) — the rest waiting here in order (load-resilience
 * lr6, LR007), so a client never meets the peer's refusal of one more channel. io lane only.
 */
class InFlightLimit {
public:
  explicit InFlightLimit(size_t limit) : limit_(limit) {}

  struct Waiter {
    std::function<bool()> live;  ///< false once it gave up waiting (skipped)
    std::function<void()> start; ///< starts the request; it calls release() when it ends
  };

  /** Start `waiter` now if `peer` has room, else queue it. */
  void run(const std::string &peer, Waiter waiter);
  /** A started request to `peer` ended: start the next live waiter. */
  void release(const std::string &peer);

  size_t inFlight(const std::string &peer) const;
  size_t waiting(const std::string &peer) const;

private:
  struct Slot {
    size_t inFlight{0};
    std::deque<Waiter> waiting;
  };

  size_t limit_;
  std::unordered_map<std::string, Slot> slots_;
};

} // namespace pp::network

#endif // PP_LEDGER_IN_FLIGHT_LIMIT_H
