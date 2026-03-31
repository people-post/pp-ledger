#include "Meta.h"
#include "BinaryPack.hpp"

#include <type_traits>
#include <utility>

namespace pp::common {

Meta::ValueWire Meta::valueToWire(const Value &v) {
  return std::visit(
      [](const auto &x) -> Meta::ValueWire {
        using V = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<V, int64_t>) {
          return {ValueWire::TAG_I64, pp::utl::binaryPack(x)};
        }
        if constexpr (std::is_same_v<V, uint64_t>) {
          return {ValueWire::TAG_U64, pp::utl::binaryPack(x)};
        }
        if constexpr (std::is_same_v<V, bool>) {
          return {ValueWire::TAG_BOOL, pp::utl::binaryPack(x)};
        }
        if constexpr (std::is_same_v<V, double>) {
          return {ValueWire::TAG_DOUBLE, pp::utl::binaryPack(x)};
        }
        if constexpr (std::is_same_v<V, std::string>) {
          return {ValueWire::TAG_STRING, pp::utl::binaryPack(x)};
        }
        if constexpr (std::is_same_v<V, MetaPtr>) {
          return {ValueWire::TAG_META, x ? pp::utl::binaryPack(*x) : std::string()};
        }
        return {};
      },
      v);
}

bool Meta::wireToValue(const Meta::ValueWire &w, std::optional<Value> &out) {
  out = std::nullopt;
  switch (w.tag) {
  case ValueWire::TAG_I64: {
    auto r = pp::utl::binaryUnpack<int64_t>(w.payload);
    if (!r.isOk()) {
      return false;
    }
    out = r.value();
    return true;
  }
  case ValueWire::TAG_U64: {
    auto r = pp::utl::binaryUnpack<uint64_t>(w.payload);
    if (!r.isOk()) {
      return false;
    }
    out = r.value();
    return true;
  }
  case ValueWire::TAG_BOOL: {
    auto r = pp::utl::binaryUnpack<bool>(w.payload);
    if (!r.isOk()) {
      return false;
    }
    out = r.value();
    return true;
  }
  case ValueWire::TAG_DOUBLE: {
    auto r = pp::utl::binaryUnpack<double>(w.payload);
    if (!r.isOk()) {
      return false;
    }
    out = r.value();
    return true;
  }
  case ValueWire::TAG_STRING: {
    auto r = pp::utl::binaryUnpack<std::string>(w.payload);
    if (!r.isOk()) {
      return false;
    }
    out = std::move(r.value());
    return true;
  }
  case ValueWire::TAG_META: {
    auto nested = std::make_shared<Meta>();
    if (!w.payload.empty()) {
      auto r = pp::utl::binaryUnpack<Meta>(w.payload);
      if (!r.isOk()) {
        return false;
      }
      *nested = std::move(r.value());
    }
    out = std::move(nested);
    return true;
  }
  default:
    return true;
  }
}

void Meta::set(const std::string &key, const Meta &v) {
  entries_[key] = std::make_shared<Meta>(v);
}

std::optional<std::reference_wrapper<const Meta>>
Meta::getMetaIf(const std::string &key) const {
  auto it = entries_.find(key);
  if (it == entries_.end()) {
    return std::nullopt;
  }
  const auto *p = std::get_if<MetaPtr>(&it->second);
  if (!p || !(*p)) {
    return std::nullopt;
  }
  return std::cref(**p);
}

bool Meta::operator==(const Meta &other) const {
  if (entries_.size() != other.entries_.size()) {
    return false;
  }
  auto itA = entries_.begin();
  auto itB = other.entries_.begin();
  for (; itA != entries_.end(); ++itA, ++itB) {
    if (itA->first != itB->first) {
      return false;
    }
    const Value &a = itA->second;
    const Value &b = itB->second;
    if (a.index() != b.index()) {
      return false;
    }
    const bool same = std::visit(
        [&](const auto &va) -> bool {
          using V = std::decay_t<decltype(va)>;
          const auto *vb = std::get_if<V>(&b);
          if (!vb) {
            return false;
          }
          if constexpr (std::is_same_v<V, MetaPtr>) {
            if (!va && !(*vb)) {
              return true;
            }
            if (!va || !(*vb)) {
              return false;
            }
            return *va == **vb;
          } else {
            return va == *vb;
          }
        },
        a);
    if (!same) {
      return false;
    }
  }
  return true;
}

} // namespace pp::common

