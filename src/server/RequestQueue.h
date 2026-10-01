#ifndef PP_LEDGER_REQUEST_QUEUE_H
#define PP_LEDGER_REQUEST_QUEUE_H

#include "../network/AmpLedgerServer.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace pp {

/**
 * Bounded inbox of RPC requests for a role's server thread.
 *
 * The io lane pushes; the server thread pops and handles one request at a
 * time, so role state needs no locks. See docs/architecture/THREADING.md.
 */
class RequestQueue {
public:
  using Clock = std::chrono::steady_clock;
  using Reply = network::AmpLedgerServer::Reply;

  struct Item {
    std::string body;
    Reply reply;
    Clock::time_point enqueuedAt;
  };

  explicit RequestQueue(size_t capacity);

  /** Any thread. False when full or closed; the caller must still reply. */
  bool push(std::string body, Reply reply);

  /** Server thread. Next request, or nullopt at `deadline` / when closed and empty. */
  std::optional<Item> popUntil(Clock::time_point deadline);

  /** Stop accepting requests; returns those still pending (caller replies). */
  std::deque<Item> close();

  size_t size() const;
  bool isClosed() const;

private:
  const size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<Item> items_;
  bool closed_{false};
};

} // namespace pp

#endif // PP_LEDGER_REQUEST_QUEUE_H
