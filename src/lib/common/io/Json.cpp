#include "io/Json.h"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace pp::common::io {
namespace {

pp::Error makeErr(std::string message, size_t offset, std::string path) {
  std::string full = std::move(message);
  if (!path.empty()) {
    full += " at ";
    full += path;
  }
  full += " (offset ";
  full += std::to_string(offset);
  full += ")";
  return pp::Error(std::move(full));
}

void appendUtf8(std::string &out, uint32_t cp) {
  if (cp <= 0x7F) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

void appendEscapedJsonString(std::string &out, std::string_view s) {
  out.push_back('"');
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c == '"') {
      out += "\\\"";
      ++i;
    } else if (c == '\\') {
      out += "\\\\";
      ++i;
    } else if (c == '\b') {
      out += "\\b";
      ++i;
    } else if (c == '\f') {
      out += "\\f";
      ++i;
    } else if (c == '\n') {
      out += "\\n";
      ++i;
    } else if (c == '\r') {
      out += "\\r";
      ++i;
    } else if (c == '\t') {
      out += "\\t";
      ++i;
    } else if (c < 0x20) {
      static const char *hex = "0123456789abcdef";
      out += "\\u00";
      out.push_back(hex[(c >> 4) & 0xf]);
      out.push_back(hex[c & 0xf]);
      ++i;
    } else {
      // Pass through UTF-8 bytes as-is (already valid UTF-8 from our parsers/builders).
      out.push_back(static_cast<char>(c));
      ++i;
    }
  }
  out.push_back('"');
}

void appendNlIndent(std::string &o, int indent, int depth) {
  o.push_back('\n');
  if (indent > 0 && depth > 0) {
    o.append(static_cast<size_t>(indent * depth), ' ');
  }
}

pp::Roe<void> appendJsonValue(std::string &o, const Value &value, int indent,
                              int depth, const std::string &path);

pp::Roe<void> appendJsonObject(std::string &o, const Object &m, int indent,
                               int depth, const std::string &path) {
  o.push_back('{');
  if (m.empty()) {
    o.push_back('}');
    return {};
  }
  const bool pretty = indent >= 0;
  bool first = true;
  for (const auto &[k, val] : m.fields()) {
    if (!first) {
      o.push_back(',');
    }
    first = false;
    if (pretty) {
      appendNlIndent(o, indent, depth + 1);
    }
    appendEscapedJsonString(o, k);
    o.push_back(':');
    if (pretty) {
      o.push_back(' ');
    }
    std::string childPath = path;
    if (childPath.empty()) {
      childPath = "/";
    }
    if (childPath.back() != '/') {
      childPath.push_back('/');
    }
    childPath += k;
    auto r = appendJsonValue(o, val, indent, depth + 1, childPath);
    if (!r.isOk()) {
      return r;
    }
  }
  if (pretty) {
    appendNlIndent(o, indent, depth);
  }
  o.push_back('}');
  return {};
}

pp::Roe<void> appendJsonValue(std::string &o, const Value &value, int indent,
                              int depth, const std::string &path) {
  return std::visit(
      [&](const auto &v) -> pp::Roe<void> {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, Null>) {
          o += "null";
          return {};
        }
        if constexpr (std::is_same_v<V, int64_t>) {
          o += std::to_string(v);
          return {};
        }
        if constexpr (std::is_same_v<V, uint64_t>) {
          if (v > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            return makeErr("uint64 value exceeds int64 range for JSON", 0, path);
          }
          o += std::to_string(v);
          return {};
        }
        if constexpr (std::is_same_v<V, bool>) {
          o += v ? "true" : "false";
          return {};
        }
        if constexpr (std::is_same_v<V, double>) {
          if (!std::isfinite(v)) {
            return makeErr("non-finite double cannot be JSON", 0, path);
          }
          std::ostringstream oss;
          oss << std::setprecision(17) << std::defaultfloat << v;
          o += oss.str();
          return {};
        }
        if constexpr (std::is_same_v<V, std::string>) {
          appendEscapedJsonString(o, v);
          return {};
        }
        if constexpr (std::is_same_v<V, ObjectPtr>) {
          if (!v) {
            return makeErr("null ObjectPtr is invalid; use Null", 0, path);
          }
          return appendJsonObject(o, *v, indent, depth, path);
        }
        if constexpr (std::is_same_v<V, ArrayPtr>) {
          if (!v) {
            return makeErr("null ArrayPtr is invalid; use Null", 0, path);
          }
          o.push_back('[');
          const bool pretty = indent >= 0;
          for (size_t i = 0; i < v->elements.size(); ++i) {
            if (i > 0) {
              o.push_back(',');
            }
            if (pretty) {
              appendNlIndent(o, indent, depth + 1);
            }
            auto r = appendJsonValue(o, v->elements[i], indent, depth + 1,
                                     path + "/" + std::to_string(i));
            if (!r.isOk()) {
              return r;
            }
          }
          if (pretty && !v->elements.empty()) {
            appendNlIndent(o, indent, depth);
          }
          o.push_back(']');
          return {};
        }
        return makeErr("unknown value type", 0, path);
      },
      value);
}

class Parser {
public:
  explicit Parser(std::string_view s)
      : start_(s.data()), p_(s.data()), end_(s.data() + s.size()) {}

  size_t offset() const { return static_cast<size_t>(p_ - start_); }

  void skipWs() {
    while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) {
      ++p_;
    }
  }

  bool atEnd() const { return p_ >= end_; }

  pp::Roe<Value> parseValue(const std::string &path) {
    skipWs();
    char c = peek();
    if (c == '"') {
      auto s = parseString(path);
      if (!s.isOk()) {
        return s.error();
      }
      return Value(std::move(s.value()));
    }
    if (c == '{') {
      auto obj = parseObject(path);
      if (!obj.isOk()) {
        return obj.error();
      }
      return Value(std::make_shared<Object>(std::move(obj.value())));
    }
    if (c == '[') {
      return parseArray(path);
    }
    if (c == 'n' && static_cast<size_t>(end_ - p_) >= 4 &&
        std::string_view(p_, 4) == "null") {
      p_ += 4;
      return Value(Null{});
    }
    if (c == 't' && static_cast<size_t>(end_ - p_) >= 4 &&
        std::string_view(p_, 4) == "true") {
      p_ += 4;
      return Value(true);
    }
    if (c == 'f' && static_cast<size_t>(end_ - p_) >= 5 &&
        std::string_view(p_, 5) == "false") {
      p_ += 5;
      return Value(false);
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return parseNumber(path);
    }
    return makeErr("expected value", offset(), path);
  }

private:
  const char *start_;
  const char *p_;
  const char *end_;

  char peek() const { return p_ < end_ ? *p_ : '\0'; }

  bool tryConsume(char c) {
    skipWs();
    if (peek() != c) {
      return false;
    }
    ++p_;
    return true;
  }

  pp::Roe<void> expect(char c, const std::string &path) {
    skipWs();
    if (peek() != c) {
      return makeErr(std::string("expected '") + c + "'", offset(), path);
    }
    ++p_;
    return {};
  }

  static int hexVal(char h) {
    if (h >= '0' && h <= '9') {
      return h - '0';
    }
    if (h >= 'A' && h <= 'F') {
      return h - 'A' + 10;
    }
    if (h >= 'a' && h <= 'f') {
      return h - 'a' + 10;
    }
    return -1;
  }

  pp::Roe<uint32_t> parseHex4(const std::string &path) {
    if (end_ - p_ < 4) {
      return makeErr("truncated \\u escape", offset(), path);
    }
    uint32_t cp = 0;
    for (int i = 0; i < 4; ++i) {
      int dv = hexVal(*p_++);
      if (dv < 0) {
        return makeErr("invalid hex in \\u escape", offset(), path);
      }
      cp = (cp << 4) | static_cast<uint32_t>(dv);
    }
    return cp;
  }

  pp::Roe<std::string> parseString(const std::string &path) {
    std::string out;
    auto ex = expect('"', path);
    if (!ex.isOk()) {
      return ex.error();
    }
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"') {
        return out;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return makeErr("unescaped control character in string", offset(), path);
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (p_ >= end_) {
        return makeErr("truncated escape", offset(), path);
      }
      char e = *p_++;
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out.push_back(e);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        auto cu = parseHex4(path);
        if (!cu.isOk()) {
          return cu.error();
        }
        uint32_t cp = cu.value();
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          if (p_ + 1 < end_ && p_[0] == '\\' && p_[1] == 'u') {
            p_ += 2;
            auto cl = parseHex4(path);
            if (!cl.isOk()) {
              return cl.error();
            }
            uint32_t low = cl.value();
            if (low < 0xDC00 || low > 0xDFFF) {
              return makeErr("invalid UTF-16 surrogate pair", offset(), path);
            }
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else {
            return makeErr("missing low surrogate in \\u escape", offset(), path);
          }
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return makeErr("unexpected low surrogate", offset(), path);
        }
        appendUtf8(out, cp);
        break;
      }
      default:
        return makeErr("invalid escape", offset(), path);
      }
    }
    return makeErr("unterminated string", offset(), path);
  }

  pp::Roe<Value> parseNumber(const std::string &path) {
    const char *start = p_;
    if (peek() == '-') {
      ++p_;
    }
    if (peek() == '0') {
      ++p_;
    } else if (std::isdigit(static_cast<unsigned char>(peek()))) {
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
        ++p_;
      }
    } else {
      return makeErr("invalid number", offset(), path);
    }
    bool isFloat = false;
    if (p_ < end_ && *p_ == '.') {
      isFloat = true;
      ++p_;
      if (p_ >= end_ || !std::isdigit(static_cast<unsigned char>(*p_))) {
        return makeErr("invalid fraction in number", offset(), path);
      }
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
        ++p_;
      }
    }
    if (p_ < end_ && (*p_ == 'e' || *p_ == 'E')) {
      isFloat = true;
      ++p_;
      if (p_ < end_ && (*p_ == '+' || *p_ == '-')) {
        ++p_;
      }
      if (p_ >= end_ || !std::isdigit(static_cast<unsigned char>(*p_))) {
        return makeErr("invalid exponent in number", offset(), path);
      }
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
        ++p_;
      }
    }
    std::string num(start, p_);
    if (isFloat) {
      double d = 0.0;
      auto r = std::from_chars(num.data(), num.data() + num.size(), d);
      if (r.ec != std::errc() || r.ptr != num.data() + num.size()) {
        return makeErr("invalid floating-point number", offset(), path);
      }
      return Value(d);
    }
    int64_t v = 0;
    auto r = std::from_chars(num.data(), num.data() + num.size(), v);
    if (r.ec == std::errc::result_out_of_range) {
      return makeErr("integer exceeds int64 range", offset(), path);
    }
    if (r.ec != std::errc() || r.ptr != num.data() + num.size()) {
      return makeErr("invalid integer", offset(), path);
    }
    return Value(v);
  }

  pp::Roe<Object> parseObject(const std::string &path) {
    Object out;
    auto ex = expect('{', path);
    if (!ex.isOk()) {
      return ex.error();
    }
    skipWs();
    if (tryConsume('}')) {
      return out;
    }
    for (;;) {
      skipWs();
      auto keyR = parseString(path);
      if (!keyR.isOk()) {
        return keyR.error();
      }
      std::string key = std::move(keyR.value());
      if (out.contains(key)) {
        return makeErr("duplicate object key", offset(), path + "/" + key);
      }
      auto colon = expect(':', path);
      if (!colon.isOk()) {
        return colon.error();
      }
      std::string childPath = path.empty() ? ("/" + key) : (path + "/" + key);
      auto val = parseValue(childPath);
      if (!val.isOk()) {
        return val.error();
      }
      out.set(std::move(key), std::move(val.value()));
      skipWs();
      if (tryConsume('}')) {
        return out;
      }
      auto comma = expect(',', path);
      if (!comma.isOk()) {
        return comma.error();
      }
    }
  }

  pp::Roe<Value> parseArray(const std::string &path) {
    auto ex = expect('[', path);
    if (!ex.isOk()) {
      return ex.error();
    }
    auto arr = std::make_shared<Array>();
    skipWs();
    if (tryConsume(']')) {
      return Value(ArrayPtr(std::move(arr)));
    }
    for (;;) {
      const size_t idx = arr->elements.size();
      auto elem = parseValue(path + "/" + std::to_string(idx));
      if (!elem.isOk()) {
        return elem.error();
      }
      arr->elements.push_back(std::move(elem.value()));
      skipWs();
      if (tryConsume(']')) {
        return Value(ArrayPtr(std::move(arr)));
      }
      auto comma = expect(',', path);
      if (!comma.isOk()) {
        return comma.error();
      }
    }
  }
};

} // namespace

pp::Roe<std::string> valueToJsonString(const Value &v, int indent) {
  std::string o;
  auto r = appendJsonValue(o, v, indent, 0, "");
  if (!r.isOk()) {
    return r.error();
  }
  return o;
}

pp::Roe<Value> valueFromJsonString(const std::string &json) {
  Parser p(json);
  auto v = p.parseValue("");
  if (!v.isOk()) {
    return v;
  }
  p.skipWs();
  if (!p.atEnd()) {
    return pp::Error("trailing data after JSON value (offset " +
                     std::to_string(p.offset()) + ")");
  }
  return v;
}

std::string metaToJsonString(const Meta &m, int indent) {
  auto r = valueToJsonString(Value(std::make_shared<Object>(m)), indent);
  if (!r.isOk()) {
    return std::string("{\"error\":") + "\"" + r.error().message + "\"}";
  }
  return std::move(r.value());
}

bool metaFromJsonString(Meta &out, const std::string &json) {
  auto r = valueFromJsonString(json);
  if (!r.isOk()) {
    return false;
  }
  auto *obj = std::get_if<ObjectPtr>(&r.value());
  if (!obj || !(*obj)) {
    return false;
  }
  out = **obj;
  return true;
}

} // namespace pp::common::io
