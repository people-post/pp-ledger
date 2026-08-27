#pragma once

/**
 * FiFoMap — associative container whose iteration order follows first insertion.
 *
 * Despite the name, this is not a queue: keys are looked up by equality, and
 * updating an existing key keeps its position (LinkedHashMap-style). "FiFo"
 * here means "first inserted, first in iteration," matching human-facing
 * document trees (JSON object key order).
 */

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pp::common {

template <typename K, typename V>
class FiFoMap {
public:
  using value_type = std::pair<K, V>;
  using iterator = typename std::vector<value_type>::iterator;
  using const_iterator = typename std::vector<value_type>::const_iterator;

  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }

  void clear() {
    entries_.clear();
    index_.clear();
  }

  bool contains(const K &key) const { return index_.find(key) != index_.end(); }

  /** Insert or assign. New keys append; existing keys keep insertion position. */
  void set(const K &key, V value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
      entries_[it->second].second = std::move(value);
      return;
    }
    index_.emplace(key, entries_.size());
    entries_.emplace_back(key, std::move(value));
  }

  void set(K &&key, V value) {
    auto it = index_.find(key);
    if (it != index_.end()) {
      entries_[it->second].second = std::move(value);
      return;
    }
    const size_t idx = entries_.size();
    index_.emplace(key, idx);
    entries_.emplace_back(std::move(key), std::move(value));
  }

  std::optional<std::reference_wrapper<V>> tryGet(const K &key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      return std::nullopt;
    }
    return entries_[it->second].second;
  }

  std::optional<std::reference_wrapper<const V>> tryGet(const K &key) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
      return std::nullopt;
    }
    return entries_[it->second].second;
  }

  V &at(const K &key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      throw std::out_of_range("FiFoMap::at");
    }
    return entries_[it->second].second;
  }

  const V &at(const K &key) const {
    auto it = index_.find(key);
    if (it == index_.end()) {
      throw std::out_of_range("FiFoMap::at");
    }
    return entries_[it->second].second;
  }

  bool erase(const K &key) {
    auto it = index_.find(key);
    if (it == index_.end()) {
      return false;
    }
    const size_t idx = it->second;
    index_.erase(it);
    entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(idx));
    for (auto &kv : index_) {
      if (kv.second > idx) {
        --kv.second;
      }
    }
    return true;
  }

  iterator begin() { return entries_.begin(); }
  iterator end() { return entries_.end(); }
  const_iterator begin() const { return entries_.begin(); }
  const_iterator end() const { return entries_.end(); }
  const_iterator cbegin() const { return entries_.cbegin(); }
  const_iterator cend() const { return entries_.cend(); }

  const std::vector<value_type> &entries() const { return entries_; }

  bool operator==(const FiFoMap &other) const {
    if (entries_.size() != other.entries_.size()) {
      return false;
    }
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].first != other.entries_[i].first ||
          entries_[i].second != other.entries_[i].second) {
        return false;
      }
    }
    return true;
  }

private:
  std::vector<value_type> entries_;
  std::unordered_map<K, size_t> index_;
};

} // namespace pp::common
