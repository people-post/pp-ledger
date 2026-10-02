#ifndef PP_LEDGER_TX_POOL_H
#define PP_LEDGER_TX_POOL_H

#include "../ledger/Ledger.h"

#include <cstddef>
#include <cstdint>
#include <map>
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
  };

  enum class Add { Added, Duplicate, Full };

  TxPool() : TxPool(Limits{}) {}
  explicit TxPool(Limits limits) : limits_(limits) {}

  Add add(const Ledger::Record &record, int64_t nowSec);
  /** Oldest first, at most `max`. */
  std::vector<Ledger::Record> pending(size_t max) const;
  /** Drop records a committed block included. */
  void removeIncluded(const std::vector<Ledger::Record> &records);
  /** Drop records past their validity window (or TTL); returns how many. */
  size_t expire(int64_t nowSec);

  size_t size() const { return byOrder_.size(); }

private:
  struct Entry {
    Ledger::Record record;
    std::string key;
    size_t bytes{0};
    int64_t expiresAt{0};
  };
  static std::string keyOf(const Ledger::Record &record);
  void erase(uint64_t seq);

  Limits limits_;
  uint64_t nextSeq_{0};
  size_t bytes_{0};
  std::map<uint64_t, Entry> byOrder_;
  std::unordered_map<std::string, uint64_t> byKey_;
};

} // namespace pp

#endif // PP_LEDGER_TX_POOL_H
