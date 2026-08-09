#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Serialize.hpp"

#include <type_traits>

namespace pp::common {

class Meta {
public:
  /** Tag + length-delimited payload for one Meta value (forward-compatible skip). */
  struct ValueWire {
    static constexpr uint16_t TAG_I64 = 1;
    static constexpr uint16_t TAG_U64 = 2;
    static constexpr uint16_t TAG_BOOL = 3;
    static constexpr uint16_t TAG_DOUBLE = 4;
    static constexpr uint16_t TAG_STRING = 5;
    static constexpr uint16_t TAG_META = 6;
    static constexpr uint16_t TAG_ARRAY = 7;

    uint16_t tag{ 0 };
    std::string payload;

    template <typename Archive> void serialize(Archive &ar) { ar & tag & payload; }
  };

  struct Array;
  using MetaPtr = std::shared_ptr<Meta>;
  using ArrayPtr = std::shared_ptr<Array>;

  using Value =
      std::variant<int64_t, uint64_t, bool, double, std::string, MetaPtr, ArrayPtr>;

  /** Ordered JSON array (recursive values). */
  struct Array {
    std::vector<Value> elements;
  };

  /** Build a JSON array value (e.g. signatures, records). */
  static Value array(std::vector<Value> elements) {
    auto a = std::make_shared<Array>();
    a->elements = std::move(elements);
    return Value(ArrayPtr(std::move(a)));
  }

  Meta() = default;

  bool operator==(const Meta &other) const;

  template <typename Archive> void serialize(Archive &ar) {
    uint64_t entryCount = static_cast<uint64_t>(entries_.size());
    ar & entryCount;

    if constexpr (std::is_same_v<Archive, InputArchive>) {
      std::map<std::string, Value> parsed;

      for (uint64_t i = 0; i < entryCount; ++i) {
        std::string key;
        ValueWire wire;
        ar & key & wire;
        if (ar.failed()) {
          return;
        }

        if (parsed.count(key) != 0) {
          entries_.clear();
          return;
        }

        std::optional<Value> decoded;
        if (!wireToValue(wire, decoded)) {
          entries_.clear();
          return;
        }
        if (decoded) {
          parsed.emplace(std::move(key), std::move(*decoded));
        }
      }

      entries_ = std::move(parsed);
      return;
    }

    if constexpr (std::is_same_v<Archive, OutputArchive>) {
      for (const auto &[key, value] : entries_) {
        const ValueWire wire = valueToWire(value);
        ar & key & wire;
      }
    }
  }

  bool empty() const { return entries_.empty(); }
  size_t size() const { return entries_.size(); }
  bool contains(const std::string &key) const { return entries_.count(key) != 0; }

  void clear() { entries_.clear(); }
  void erase(const std::string &key) { entries_.erase(key); }

  const std::map<std::string, Value> &entries() const { return entries_; }

  void set(const std::string &key, Value value) { entries_[key] = std::move(value); }

  void set(const std::string &key, int64_t v) { entries_[key] = v; }
  void set(const std::string &key, uint64_t v) { entries_[key] = v; }
  void set(const std::string &key, bool v) { entries_[key] = v; }
  void set(const std::string &key, double v) { entries_[key] = v; }
  void set(const std::string &key, std::string v) { entries_[key] = std::move(v); }
  void set(const std::string &key, const char *v) {
    entries_[key] = v ? std::string(v) : std::string();
  }
  void set(const std::string &key, const Meta &v);

  template <typename T>
  std::optional<T> getIf(const std::string &key) const {
    auto it = entries_.find(key);
    if (it == entries_.end()) {
      return std::nullopt;
    }
    const auto *p = std::get_if<T>(&it->second);
    if (!p) {
      return std::nullopt;
    }
    return *p;
  }

  /** Same resolution as getIf<T>; missing or wrong type → defaultValue. */
  template <typename T>
  T getOrDefault(const std::string &key, T defaultValue) const {
    if (auto v = getIf<T>(key)) {
      return *v;
    }
    return defaultValue;
  }

  std::optional<std::reference_wrapper<const Meta>> getMetaIf(const std::string &key) const;

private:
  static bool valueEqual(const Value &a, const Value &b);
  static ValueWire valueToWire(const Value &v);
  /** Returns false on malformed payload for a known tag. Unknown tag: out=nullopt, true. */
  static bool wireToValue(const ValueWire &w, std::optional<Value> &out);

  std::map<std::string, Value> entries_;
};

} // namespace pp::common
