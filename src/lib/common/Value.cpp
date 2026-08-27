#include "Value.h"
#include "BinaryPack.hpp"

#include <limits>
#include <sstream>
#include <string>
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

std::optional<uint64_t> Object::getNonNegInt(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return std::nullopt;
  }
  return asNonNegInt(slot->get());
}

std::optional<std::string> Object::getString(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return std::nullopt;
  }
  return asString(slot->get());
}

const Object *Object::getObject(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return nullptr;
  }
  return asObject(slot->get());
}

const Array *Object::getArray(const std::string &key) const {
  auto slot = fields_.tryGet(key);
  if (!slot) {
    return nullptr;
  }
  return asArray(slot->get());
}

bool Object::setJsonUInt(const std::string &key, uint64_t v) {
  if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return false;
  }
  fields_.set(key, Value(static_cast<int64_t>(v)));
  return true;
}

void Object::setUIntForJson(const std::string &key, uint64_t v) {
  if (!setJsonUInt(key, v)) {
    fields_.set(key, Value(std::to_string(v)));
  }
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

bool isNullValue(const Value &v) { return std::holds_alternative<Null>(v); }

bool isStringValue(const Value &v) {
  return std::holds_alternative<std::string>(v);
}

bool isObjectValue(const Value &v) {
  const auto *p = std::get_if<ObjectPtr>(&v);
  return p && *p;
}

bool isArrayValue(const Value &v) {
  const auto *p = std::get_if<ArrayPtr>(&v);
  return p && *p;
}

bool isBoolValue(const Value &v) { return std::holds_alternative<bool>(v); }

std::optional<uint64_t> asNonNegInt(const Value &v) {
  if (const auto *i = std::get_if<int64_t>(&v)) {
    if (*i < 0) {
      return std::nullopt;
    }
    return static_cast<uint64_t>(*i);
  }
  if (const auto *u = std::get_if<uint64_t>(&v)) {
    return *u;
  }
  if (const auto *s = std::get_if<std::string>(&v)) {
    if (s->empty()) {
      return std::nullopt;
    }
    try {
      size_t idx = 0;
      int base = 10;
      const char *p = s->c_str();
      if (s->size() >= 2 && s->at(0) == '0' &&
          (s->at(1) == 'x' || s->at(1) == 'X')) {
        p += 2;
        base = 16;
      }
      // Reject signed forms; stoull("-1") wraps on unsigned types.
      if (*p == '+' || *p == '-') {
        return std::nullopt;
      }
      if (*p == '\0') {
        return std::nullopt;
      }
      unsigned long long parsed = std::stoull(p, &idx, base);
      if (idx != std::char_traits<char>::length(p)) {
        return std::nullopt;
      }
      return static_cast<uint64_t>(parsed);
    } catch (...) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::string> asString(const Value &v) {
  if (const auto *s = std::get_if<std::string>(&v)) {
    return *s;
  }
  return std::nullopt;
}

const Object *asObject(const Value &v) {
  const auto *p = std::get_if<ObjectPtr>(&v);
  if (!p || !(*p)) {
    return nullptr;
  }
  return p->get();
}

Object *asObject(Value &v) {
  auto *p = std::get_if<ObjectPtr>(&v);
  if (!p || !(*p)) {
    return nullptr;
  }
  return p->get();
}

const Array *asArray(const Value &v) {
  const auto *p = std::get_if<ArrayPtr>(&v);
  if (!p || !(*p)) {
    return nullptr;
  }
  return p->get();
}

Array *asArray(Value &v) {
  auto *p = std::get_if<ArrayPtr>(&v);
  if (!p || !(*p)) {
    return nullptr;
  }
  return p->get();
}

} // namespace pp::common
