#ifndef PP_LEDGER_BLOCK_WAIT_LIST_H
#define PP_LEDGER_BLOCK_WAIT_LIST_H

#include "RequestQueue.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * Parked BLOCK_WAIT requests (server thread only). A downstream asks "tell me
 * when your next block id passes N"; the reply is held until the tip moves or
 * the hold ends. One wait per peer: a newer one answers the older at once.
 * Replies are returned for the caller to send, so this holds no I/O.
 */
class BlockWaitList {
public:
  using Clock = RequestQueue::Clock;
  using Reply = RequestQueue::Reply;

  explicit BlockWaitList(size_t capacity) : capacity_(capacity) {}

  /**
   * Hold `reply` until the tip passes `knownNext` or `deadline`. Returns the
   * replies due now: this one if the tip is already past (or the list is
   * full), and the peer's previous wait.
   */
  std::vector<Reply> park(const std::string &peerId, uint64_t knownNext, Clock::time_point deadline, Reply reply,
                          uint64_t tip);

  /** Replies due: waits the tip has passed, and those past their deadline. */
  std::vector<Reply> poll(uint64_t tip, Clock::time_point now);

  /** Every parked reply (stopping). */
  std::vector<Reply> takeAll();

  size_t size() const { return waits_.size(); }

private:
  struct Wait {
    uint64_t knownNext{0};
    Clock::time_point deadline;
    Reply reply;
  };
  size_t capacity_;
  std::unordered_map<std::string, Wait> waits_;
};

} // namespace pp

#endif // PP_LEDGER_BLOCK_WAIT_LIST_H
