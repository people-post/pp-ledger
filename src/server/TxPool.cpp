#include "ledger/server/TxPool.h"

#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"

#include <algorithm>
#include <type_traits>
#include <variant>

namespace pp {
namespace {

/** The transaction's own validity end, if it has one (TxIdempotencyWindow). */
int64_t validUntil(const Ledger::Record &record) {
  auto typed = record.decode();
  if (!typed) {
    return 0;
  }
  return std::visit(
      [](const auto &tx) -> int64_t {
        using T = std::decay_t<decltype(tx)>;
        if constexpr (std::is_base_of_v<Ledger::TxIdempotencyWindow, T>) {
          return tx.validationTsMax;
        } else {
          return 0;
        }
      },
      typed.value());
}

} // namespace

std::string TxPool::keyOf(const Ledger::Record &record) { return utl::sha256Raw(utl::binaryPack(record)); }

TxPool::Add TxPool::add(const Ledger::Record &record, int64_t nowSec, std::optional<uint64_t> sender) {
  std::string key = keyOf(record);
  if (byKey_.contains(key)) {
    ++counters_.duplicates;
    return Add::Duplicate;
  }
  size_t bytes = record.data.size();
  for (const auto &sig : record.signatures) {
    bytes += sig.size();
  }
  if (byOrder_.size() >= limits_.maxRecords || bytes_ + bytes > limits_.maxBytes) {
    ++counters_.full;
    return Add::Full;
  }
  if (sender && pooledBy(*sender) >= limits_.maxPerSender) {
    ++counters_.senderFull;
    return Add::SenderFull;
  }
  const int64_t windowEnd = validUntil(record);
  const int64_t ttlEnd = nowSec + limits_.ttlSeconds;
  const uint64_t seq = nextSeq_++;
  byKey_.emplace(key, seq);
  byOrder_.emplace(seq, Entry{record, std::move(key), bytes, nowSec,
                              windowEnd > 0 ? std::min(windowEnd, ttlEnd) : ttlEnd, sender});
  if (sender) {
    ++bySender_[*sender];
  }
  bytes_ += bytes;
  ++counters_.added;
  return Add::Added;
}

std::vector<Ledger::Record> TxPool::pending(size_t max, size_t maxBytes) const {
  std::vector<Ledger::Record> out;
  size_t bytes = 0;
  for (auto it = byOrder_.begin(); it != byOrder_.end() && out.size() < max; ++it) {
    const size_t recordBytes = utl::binaryPack(it->second.record).size();
    if (bytes + recordBytes > maxBytes) {
      break; // oldest first, in order: the rest wait for the next pull
    }
    bytes += recordBytes;
    out.push_back(it->second.record);
  }
  return out;
}

void TxPool::erase(uint64_t seq) {
  auto it = byOrder_.find(seq);
  if (it == byOrder_.end()) {
    return;
  }
  bytes_ -= it->second.bytes;
  byKey_.erase(it->second.key);
  if (const auto &sender = it->second.sender) {
    if (auto s = bySender_.find(*sender); s != bySender_.end() && --s->second == 0) {
      bySender_.erase(s);
    }
  }
  byOrder_.erase(it);
}

void TxPool::removeIncluded(const std::vector<Ledger::Record> &records) {
  for (const auto &record : records) {
    if (auto it = byKey_.find(keyOf(record)); it != byKey_.end()) {
      erase(it->second);
      ++counters_.included;
    }
  }
}

size_t TxPool::expire(int64_t nowSec) {
  std::vector<uint64_t> expired;
  for (const auto &[seq, entry] : byOrder_) {
    if (nowSec > entry.expiresAt) {
      expired.push_back(seq);
    }
  }
  for (uint64_t seq : expired) {
    erase(seq);
  }
  counters_.expired += expired.size();
  return expired.size();
}

size_t TxPool::removeIf(const std::function<bool(const Ledger::Record &)> &drop) {
  std::vector<uint64_t> doomed;
  for (const auto &[seq, entry] : byOrder_) {
    if (drop(entry.record)) {
      doomed.push_back(seq);
    }
  }
  for (uint64_t seq : doomed) {
    erase(seq);
  }
  counters_.dropped += doomed.size();
  return doomed.size();
}

size_t TxPool::pooledBy(uint64_t sender) const {
  auto it = bySender_.find(sender);
  return it == bySender_.end() ? 0 : it->second;
}

std::optional<int64_t> TxPool::oldestAgeSeconds(int64_t nowSec) const {
  if (byOrder_.empty()) {
    return std::nullopt;
  }
  return std::max<int64_t>(0, nowSec - byOrder_.begin()->second.addedAt);
}

} // namespace pp
