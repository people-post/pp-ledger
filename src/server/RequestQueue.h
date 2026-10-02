#ifndef PP_LEDGER_REQUEST_QUEUE_H
#define PP_LEDGER_REQUEST_QUEUE_H

#include "../network/AmpLedgerServer.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace pp {

/**
 * Bounded inbox of RPC requests for a role's server thread.
 *
 * The io lane pushes; the server thread pops and handles one request at a
 * time, so role state needs no locks. The same queue carries completion tasks
 * (results of a role's outbound calls) back to the server thread. See
 * docs/architecture/THREADING.md.
 *
 * Two lanes: Normal (the node's own work: writes, registration, timing, and
 * completion tasks) and Low (reads such as block sync). Low has its own,
 * smaller capacity and a per-peer cap, so a flood of reads gets "busy"
 * without crowding out normal work; it is served at least once every
 * kNormalPerLow + 1 pops so reads never starve.
 */
class RequestQueue {
public:
  using Clock = std::chrono::steady_clock;
  using Reply = network::AmpLedgerServer::Reply;

  enum class Lane { Normal, Low };

  /** While both lanes have work, serve this many Normal items per Low one. */
  static constexpr size_t kNormalPerLow = 4;
  /** Low-lane requests one peer may have queued at once. */
  static constexpr size_t kLowPerPeer = 16;

  /** A request (sender, body, reply), or a completion task when `task` is set. */
  struct Item {
    /** Sender's authenticated AMP peer id ("" when unknown, e.g. in-process). */
    std::string peerId;
    std::string body;
    Reply reply;
    Clock::time_point enqueuedAt;
    std::function<void()> task;
    Lane lane{Lane::Normal};
  };

  /** `capacity` bounds the Normal lane; the Low lane holds a quarter of it (at least 1). */
  explicit RequestQueue(size_t capacity);

  /** Any thread. False when the lane (or the peer's Low share) is full, or closed; the caller must still reply. */
  bool push(std::string peerId, std::string body, Reply reply, Lane lane = Lane::Normal);

  /** Any thread. Completion task; not subject to capacity. False once closed. */
  bool pushTask(std::function<void()> task);

  /** Server thread. Next request, or nullopt at `deadline` / when closed and empty. */
  std::optional<Item> popUntil(Clock::time_point deadline);

  /** Server thread. Next completion task (requests stay queued), or nullopt at `deadline`. */
  std::optional<Item> popTaskUntil(Clock::time_point deadline);

  /** Stop accepting requests; returns those still pending (caller replies). */
  std::deque<Item> close();

  size_t size() const;
  bool isClosed() const;

private:
  const size_t capacity_;
  const size_t lowCapacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> normal_;
  std::deque<Item> low_;
  std::unordered_map<std::string, size_t> lowByPeer_;
  size_t normalStreak_{0};
  bool closed_{false};
};

} // namespace pp

#endif // PP_LEDGER_REQUEST_QUEUE_H
