#pragma once

/**
 * Human-friendly in-memory document tree (JSON-shaped intermediate unit).
 * Not a canonical identity / signing type — real structs own comparison protocol.
 *
 * Value is the document root. Object is a FiFoMap of string → Value (insertion order
 * shared by memory, JSON, and binary wire).
 */

#include "FiFoMap.h"
#include "Serialize.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace pp::common {

struct Null {
  bool operator==(const Null &) const { return true; }
};

class Object;
struct Array;

using ObjectPtr = std::shared_ptr<Object>;
using ArrayPtr = std::shared_ptr<Array>;

using Value = std::variant<Null, int64_t, uint64_t, bool, double, std::string,
                           ObjectPtr, ArrayPtr>;

struct Array {
  std::vector<Value> elements;
  bool operator==(const Array &o) const;
};

/** Tag + length-delimited payload for one Value (forward-compatible skip). */
struct ValueWire {
  static constexpr uint16_t TAG_I64 = 1;
  static constexpr uint16_t TAG_U64 = 2;
  static constexpr uint16_t TAG_BOOL = 3;
  static constexpr uint16_t TAG_DOUBLE = 4;
  static constexpr uint16_t TAG_STRING = 5;
  static constexpr uint16_t TAG_OBJECT = 6;
  static constexpr uint16_t TAG_ARRAY = 7;
  static constexpr uint16_t TAG_NULL = 8;

  uint16_t tag{0};
  std::string payload;

  template <typename Archive> void serialize(Archive &ar) { ar & tag & payload; }
};

bool valueEqual(const Value &a, const Value &b);
ValueWire valueToWire(const Value &v);
/** false on malformed known tag. Unknown tag: out=nullopt, true (skip). */
bool wireToValue(const ValueWire &w, std::optional<Value> &out);

Value makeArray(std::vector<Value> elements);

class Object {
public:
  /** Nested aliases so Meta::Value / Meta::Array keep compiling. */
  using Value = pp::common::Value;
  using Array = pp::common::Array;
  using ArrayPtr = pp::common::ArrayPtr;
  using MetaPtr = pp::common::ObjectPtr;

  Object() = default;

  bool operator==(const Object &other) const;

  template <typename Archive> void serialize(Archive &ar) {
    uint64_t entryCount = static_cast<uint64_t>(fields_.size());
    ar & entryCount;

    if constexpr (std::is_same_v<Archive, InputArchive>) {
      fields_.clear();
      for (uint64_t i = 0; i < entryCount; ++i) {
        std::string key;
        ValueWire wire;
        ar & key & wire;
        if (ar.failed()) {
          fields_.clear();
          return;
        }
        if (fields_.contains(key)) {
          fields_.clear();
          return;
        }
        std::optional<Value> decoded;
        if (!wireToValue(wire, decoded)) {
          fields_.clear();
          return;
        }
        if (decoded) {
          fields_.set(std::move(key), std::move(*decoded));
        }
      }
      return;
    }

    if constexpr (std::is_same_v<Archive, OutputArchive>) {
      for (const auto &[key, value] : fields_) {
        const ValueWire wire = valueToWire(value);
        ar & key & wire;
      }
    }
  }

  bool empty() const { return fields_.empty(); }
  size_t size() const { return fields_.size(); }
  bool contains(const std::string &key) const { return fields_.contains(key); }

  void clear() { fields_.clear(); }
  void erase(const std::string &key) { fields_.erase(key); }

  const FiFoMap<std::string, Value> &fields() const { return fields_; }

  /** Compatibility alias for older Meta::entries(). */
  const FiFoMap<std::string, Value> &entries() const { return fields_; }

  void set(const std::string &key, Value value) {
    fields_.set(key, std::move(value));
  }

  void set(const std::string &key, Null) { fields_.set(key, Value(Null{})); }
  void set(const std::string &key, int64_t v) { fields_.set(key, Value(v)); }
  void set(const std::string &key, uint64_t v) { fields_.set(key, Value(v)); }
  void set(const std::string &key, bool v) { fields_.set(key, Value(v)); }
  void set(const std::string &key, double v) { fields_.set(key, Value(v)); }
  void set(const std::string &key, std::string v) {
    fields_.set(key, Value(std::move(v)));
  }
  void set(const std::string &key, const char *v) {
    fields_.set(key, Value(v ? std::string(v) : std::string()));
  }
  void set(const std::string &key, const Object &v);

  static Value array(std::vector<Value> elements) {
    return makeArray(std::move(elements));
  }

  template <typename T>
  std::optional<T> getIf(const std::string &key) const {
    auto slot = fields_.tryGet(key);
    if (!slot) {
      return std::nullopt;
    }
    const auto *p = std::get_if<T>(&slot->get());
    if (!p) {
      return std::nullopt;
    }
    return *p;
  }

  template <typename T>
  T getOrDefault(const std::string &key, T defaultValue) const {
    if (auto v = getIf<T>(key)) {
      return *v;
    }
    return defaultValue;
  }

  std::optional<std::reference_wrapper<const Object>>
  getMetaIf(const std::string &key) const;

  bool isNull(const std::string &key) const;

private:
  FiFoMap<std::string, Value> fields_;
};

/** Transition alias: historical Meta name = Object. */
using Meta = Object;
using MetaPtr = ObjectPtr;

} // namespace pp::common
