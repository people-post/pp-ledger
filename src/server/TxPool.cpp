#include "TxPool.h"

#include "lib/common/BinaryPack.hpp"
#include "lib/common/Utilities.h"

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

TxPool::Add TxPool::add(const Ledger::Record &record, int64_t nowSec) {
  std::string key = keyOf(record);
  if (byKey_.contains(key)) {
    return Add::Duplicate;
  }
  size_t bytes = record.data.size();
  for (const auto &sig : record.signatures) {
    bytes += sig.size();
  }
  if (byOrder_.size() >= limits_.maxRecords || bytes_ + bytes > limits_.maxBytes) {
    return Add::Full;
  }
  const int64_t windowEnd = validUntil(record);
  const int64_t ttlEnd = nowSec + limits_.ttlSeconds;
  const uint64_t seq = nextSeq_++;
  byKey_.emplace(key, seq);
  byOrder_.emplace(seq, Entry{record, std::move(key), bytes, windowEnd > 0 ? std::min(windowEnd, ttlEnd) : ttlEnd});
  bytes_ += bytes;
  return Add::Added;
}

std::vector<Ledger::Record> TxPool::pending(size_t max) const {
  std::vector<Ledger::Record> out;
  for (auto it = byOrder_.begin(); it != byOrder_.end() && out.size() < max; ++it) {
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
  byOrder_.erase(it);
}

void TxPool::removeIncluded(const std::vector<Ledger::Record> &records) {
  for (const auto &record : records) {
    if (auto it = byKey_.find(keyOf(record)); it != byKey_.end()) {
      erase(it->second);
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
  return expired.size();
}

} // namespace pp
