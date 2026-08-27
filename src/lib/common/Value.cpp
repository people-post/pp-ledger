#include "Value.h"
#include "BinaryPack.hpp"

#include <sstream>
#include <type_traits>
#include <utility>

namespace pp::common {

Value makeArray(std::vector<Value> elements) {
  auto a = std::make_shared<Array>();
  a->elements = std::move(elements);
  return Value(ArrayPtr(std::move(a)));
}

bool valueEqual(const Value &a, const Value &b) {
  if (a.index() != b.index()) {
    return false;
  }
  return std::visit(
      [&](const auto &va) -> bool {
        using V = std::decay_t<decltype(va)>;
        const auto *vb = std::get_if<V>(&b);
        if (!vb) {
          return false;
        }
        if constexpr (std::is_same_v<V, ObjectPtr>) {
          if (!va && !(*vb)) {
            return true;
          }
          if (!va || !(*vb)) {
            return false;
          }
          return *va == **vb;
        }
        if constexpr (std::is_same_v<V, ArrayPtr>) {
          if (!va && !(*vb)) {
            return true;
          }
          if (!va || !(*vb)) {
            return false;
          }
          if (va->elements.size() != (*vb)->elements.size()) {
            return false;
          }
          for (size_t i = 0; i < va->elements.size(); ++i) {
            if (!valueEqual(va->elements[i], (*vb)->elements[i])) {
              return false;
            }
          }
          return true;
        }
        return va == *vb;
      },
      a);
}

bool Array::operator==(const Array &o) const {
  if (elements.size() != o.elements.size()) {
    return false;
  }
  for (size_t i = 0; i < elements.size(); ++i) {
    if (!valueEqual(elements[i], o.elements[i])) {
      return false;
    }
  }
  return true;
}

ValueWire valueToWire(const Value &v) {
  return std::visit(
      [](const auto &x) -> ValueWire {
        using V = std::decay_t<decltype(x)>;
        if constexpr (std::is_same_v<V, Null>) {
          return {ValueWire::TAG_NULL, std::string()};
        }
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
        if constexpr (std::is_same_v<V, ObjectPtr>) {
          if (!x) {
            return {ValueWire::TAG_NULL, std::string()};
          }
          return {ValueWire::TAG_OBJECT, pp::utl::binaryPack(*x)};
        }
        if constexpr (std::is_same_v<V, ArrayPtr>) {
          if (!x) {
            return {ValueWire::TAG_NULL, std::string()};
          }
          std::ostringstream oss;
          OutputArchive ar(oss);
          uint64_t n = x->elements.size();
          ar & n;
          for (const auto &el : x->elements) {
            const ValueWire w = valueToWire(el);
            ar & w;
          }
          return {ValueWire::TAG_ARRAY, oss.str()};
        }
        return {};
      },
      v);
}

bool wireToValue(const ValueWire &w, std::optional<Value> &out) {
  out = std::nullopt;
  switch (w.tag) {
  case ValueWire::TAG_NULL:
    out = Null{};
    return true;
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
  case ValueWire::TAG_OBJECT: {
    auto nested = std::make_shared<Object>();
    if (!w.payload.empty()) {
      auto r = pp::utl::binaryUnpack<Object>(w.payload);
      if (!r.isOk()) {
        return false;
      }
      *nested = std::move(r.value());
    }
    out = std::move(nested);
    return true;
  }
  case ValueWire::TAG_ARRAY: {
    std::istringstream iss(w.payload, std::ios::binary);
    InputArchive ar(iss);
    uint64_t n = 0;
    ar & n;
    if (ar.failed()) {
      return false;
    }
    auto arr = std::make_shared<Array>();
    for (uint64_t i = 0; i < n; ++i) {
      ValueWire elemWire;
      ar & elemWire;
      if (ar.failed()) {
        return false;
      }
      std::optional<Value> ev;
      if (!wireToValue(elemWire, ev)) {
        return false;
      }
      if (!ev) {
        return false;
      }
      arr->elements.push_back(std::move(*ev));
    }
    out = ArrayPtr(std::move(arr));
    return true;
  }
  default:
    return true;
  }
}

void Object::set(const std::string &key, const Object &v) {
  fields_.set(key, Value(std::make_shared<Object>(v)));
}

std::optional<std::reference_wrapper<const Object>>
Object::getMetaIf(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return std::nullopt;
  }
  const auto *p = std::get_if<ObjectPtr>(&slot->get());
  if (!p || !(*p)) {
    return std::nullopt;
  }
  return std::cref(**p);
}

bool Object::isNull(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return false;
  }
  return std::holds_alternative<Null>(slot->get());
}

bool Object::operator==(const Object &other) const {
  if (fields_.size() != other.fields_.size()) {
    return false;
  }
  auto itA = fields_.begin();
  auto itB = other.fields_.begin();
  for (; itA != fields_.end(); ++itA, ++itB) {
    if (itA->first != itB->first) {
      return false;
    }
    if (!valueEqual(itA->second, itB->second)) {
      return false;
    }
  }
  return true;
}

} // namespace pp::common
