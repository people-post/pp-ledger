#ifndef PP_LEDGER_TX_POOL_H
#define PP_LEDGER_TX_POOL_H

#include "../ledger/Ledger.h"

#include <cstddef>
#include <cstdint>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pp {

/**
 * The beacon's pending transactions. Clients submit TX_ADD to any relay or the
 * beacon; it travels up to here. The slot leader pulls (TX_PULL) and includes
 * what is valid; a transaction leaves the pool only once a committed block
 * includes it, or when it expires — so one a leader missed or could not use
 * reaches the next leader. Server thread only; no I/O.
 */
class TxPool {
public:
  struct Limits {
    size_t maxRecords = 10000;
    size_t maxBytes = 16 * 1024 * 1024;
    /** Records without a validity window expire this long after arrival. */
    int64_t ttlSeconds = 600;
    /** Records one payer may have pooled at once (load-resilience lr4): no one fills the pool alone. */
    size_t maxPerSender = 1000;
  };

  enum class Add { Added, Duplicate, Full, SenderFull };

  /** What happened to submitted records, from the pool's start (NodeHealth). */
  struct Counters {
    uint64_t added{0};
    uint64_t duplicates{0};
    uint64_t full{0};
    uint64_t expired{0};
    uint64_t included{0};
    uint64_t senderFull{0}; ///< refused: its payer had its share pooled
    uint64_t dropped{0};    ///< removed: could no longer apply (removeIf)
  };

  TxPool() : TxPool(Limits{}) {}
  explicit TxPool(Limits limits) : limits_(limits) {}

  /** `sender`: the payer it counts against (maxPerSender); none for records without one. */
  Add add(const Ledger::Record &record, int64_t nowSec, std::optional<uint64_t> sender = std::nullopt);
  /** Oldest first, at most `max` records and `maxBytes` packed (stops at the first that would pass). */
  std::vector<Ledger::Record> pending(size_t max, size_t maxBytes = SIZE_MAX) const;
  /** Drop records a committed block included. */
  void removeIncluded(const std::vector<Ledger::Record> &records);
  /** Drop records past their validity window (or TTL); returns how many. */
  size_t expire(int64_t nowSec);
  /** Drop the records `drop` names (they can no longer apply); returns how many. */
  size_t removeIf(const std::function<bool(const Ledger::Record &)> &drop);
  /** Records `sender` has pooled. */
  size_t pooledBy(uint64_t sender) const;

  size_t size() const { return byOrder_.size(); }
  size_t bytes() const { return bytes_; }
  /** Seconds the oldest record has waited; none when empty. */
  std::optional<int64_t> oldestAgeSeconds(int64_t nowSec) const;
  const Counters &counters() const { return counters_; }

private:
  struct Entry {
    Ledger::Record record;
    std::string key;
    size_t bytes{0};
    int64_t addedAt{0};
    int64_t expiresAt{0};
    std::optional<uint64_t> sender;
  };
  static std::string keyOf(const Ledger::Record &record);
  void erase(uint64_t seq);

  Limits limits_;
  uint64_t nextSeq_{0};
  size_t bytes_{0};
  Counters counters_;
  std::map<uint64_t, Entry> byOrder_;
  std::unordered_map<std::string, uint64_t> byKey_;
  std::unordered_map<uint64_t, size_t> bySender_;
};

} // namespace pp

#endif // PP_LEDGER_TX_POOL_H
