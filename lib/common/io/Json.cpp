#include "io/Json.h"

#include <cctype>
#include <type_traits>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <variant>

namespace pp::common::io {
namespace {

void appendEscapedJsonString(std::string &out, std::string_view s);

// Forward — mutual recursion with metaToJsonString for nested Meta.
std::string encodeJsonValue(const Meta::Value &value);

void appendNlIndent(std::string &o, int indent, int depth) {
  o.push_back('\n');
  if (indent > 0 && depth > 0) {
    o.append(static_cast<size_t>(indent * depth), ' ');
  }
}

void metaObjectToStringPretty(std::string &o, const Meta &m, int indent, int depth);

void encodeJsonValuePretty(std::string &o, const Meta::Value &value, int indent,
                           int depth) {
  if (std::holds_alternative<Meta::MetaPtr>(value)) {
    const auto &p = std::get<Meta::MetaPtr>(value);
    if (!p) {
      o += "null";
      return;
    }
    metaObjectToStringPretty(o, *p, indent, depth);
    return;
  }
  o += encodeJsonValue(value);
}

void metaObjectToStringPretty(std::string &o, const Meta &m, int indent, int depth) {
  o.push_back('{');
  const auto &entries = m.entries();
  if (entries.empty()) {
    o.push_back('}');
    return;
  }
  auto it = entries.begin();
  const auto end = entries.end();
  appendNlIndent(o, indent, depth + 1);
  for (;;) {
    appendEscapedJsonString(o, it->first);
    o += ": ";
    encodeJsonValuePretty(o, it->second, indent, depth + 1);
    ++it;
    if (it == end) {
      break;
    }
    o.push_back(',');
    appendNlIndent(o, indent, depth + 1);
  }
  appendNlIndent(o, indent, depth);
  o.push_back('}');
}

void appendEscapedJsonString(std::string &out, std::string_view s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        static const char *hex = "0123456789abcdef";
        out += "\\u00";
        out.push_back(hex[(c >> 4) & 0xf]);
        out.push_back(hex[c & 0xf]);
      } else {
        out.push_back(static_cast<char>(c));
      }
      break;
    }
  }
  out.push_back('"');
}

std::string encodeJsonValue(const Meta::Value &value) {
  return std::visit(
      [](const auto &v) -> std::string {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, int64_t>) {
          return std::to_string(v);
        }
        if constexpr (std::is_same_v<V, uint64_t>) {
          return std::to_string(v);
        }
        if constexpr (std::is_same_v<V, bool>) {
          return v ? "true" : "false";
        }
        if constexpr (std::is_same_v<V, double>) {
          std::ostringstream oss;
          oss << std::setprecision(17) << std::defaultfloat << v;
          return oss.str();
        }
        if constexpr (std::is_same_v<V, std::string>) {
          std::string o;
          appendEscapedJsonString(o, v);
          return o;
        }
        if constexpr (std::is_same_v<V, Meta::MetaPtr>) {
          if (!v) {
            return "null";
          }
          std::string o;
          o.push_back('{');
          bool first = true;
          for (const auto &[k, val] : v->entries()) {
            if (!first) {
              o.push_back(',');
            }
            first = false;
            appendEscapedJsonString(o, k);
            o.push_back(':');
            o += encodeJsonValue(val);
          }
          o.push_back('}');
          return o;
        }
        return "null";
      },
      value);
}

class Parser {
public:
  explicit Parser(std::string_view s) : p_(s.data()), end_(s.data() + s.size()) {}

  bool parseMeta(Meta &out) {
    out.clear();
    skipWs();
    if (!consume('{')) {
      return false;
    }
    skipWs();
    if (peek() == '}') {
      ++p_;
      return true;
    }
    for (;;) {
      skipWs();
      if (!peek() || peek() != '"') {
        return false;
      }
      std::string key;
      if (!parseString(key)) {
        return false;
      }
      skipWs();
      if (!consume(':')) {
        return false;
      }
      skipWs();
      Meta::Value val;
      if (!parseValue(val)) {
        return false;
      }
      if (out.contains(key)) {
        return false;
      }
      out.set(key, std::move(val));
      skipWs();
      if (peek() == '}') {
        ++p_;
        return true;
      }
      if (!consume(',')) {
        return false;
      }
    }
  }

private:
  const char *p_;
  const char *end_;

  char peek() const {
    return p_ < end_ ? *p_ : '\0';
  }

  void skipWs() {
    while (p_ < end_ && std::isspace(static_cast<unsigned char>(*p_))) {
      ++p_;
    }
  }

  bool consume(char c) {
    skipWs();
    if (peek() != c) {
      return false;
    }
    ++p_;
    return true;
  }

  bool parseString(std::string &out) {
    out.clear();
    if (!consume('"')) {
      return false;
    }
    while (p_ < end_) {
      char c = *p_++;
      if (c == '"') {
        return true;
      }
      if (c == '\\') {
        if (p_ >= end_) {
          return false;
        }
        char e = *p_++;
        switch (e) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
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
          if (end_ - p_ < 4) {
            return false;
          }
          unsigned cp = 0;
          for (int i = 0; i < 4; ++i) {
            char h = *p_++;
            if (!std::isxdigit(static_cast<unsigned char>(h))) {
              return false;
            }
            int dv = h <= '9' ? h - '0'
                     : (h <= 'F' ? h - 'A' + 10 : h - 'a' + 10);
            cp = (cp << 4) | static_cast<unsigned>(dv);
          }
          if (cp < 256) {
            out.push_back(static_cast<char>(cp));
          } else {
            return false;
          }
          break;
        }
        default:
          return false;
        }
      } else {
        out.push_back(c);
      }
    }
    return false;
  }

  bool parseNumber(Meta::Value &out) {
    const char *start = p_;
    if (peek() == '-') {
      ++p_;
    }
    while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
      ++p_;
    }
    bool isFloat = false;
    if (p_ < end_ && *p_ == '.') {
      isFloat = true;
      ++p_;
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
      while (p_ < end_ && std::isdigit(static_cast<unsigned char>(*p_))) {
        ++p_;
      }
    }
    if (p_ == start) {
      return false;
    }
    std::string num(start, p_);
    if (isFloat) {
      double d = 0.0;
      auto r = std::from_chars(num.data(), num.data() + num.size(), d);
      if (r.ec != std::errc() || r.ptr != num.data() + num.size()) {
        return false;
      }
      out = d;
      return true;
    }
    if (num[0] == '-') {
      int64_t v = 0;
      auto r = std::from_chars(num.data(), num.data() + num.size(), v);
      if (r.ec != std::errc() || r.ptr != num.data() + num.size()) {
        return false;
      }
      out = v;
      return true;
    }
    uint64_t u = 0;
    auto ru = std::from_chars(num.data(), num.data() + num.size(), u);
    if (ru.ec != std::errc() || ru.ptr != num.data() + num.size()) {
      return false;
    }
    out = u;
    return true;
  }

  bool parseValue(Meta::Value &out) {
    skipWs();
    char c = peek();
    if (c == '"') {
      std::string s;
      if (!parseString(s)) {
        return false;
      }
      out = std::move(s);
      return true;
    }
    if (c == '{') {
      auto nested = std::make_shared<Meta>();
      if (!parseMeta(*nested)) {
        return false;
      }
      out = std::move(nested);
      return true;
    }
    if (c == 'n' && static_cast<size_t>(end_ - p_) >= 4 &&
        std::string_view(p_, 4) == "null") {
      p_ += 4;
      out = Meta::MetaPtr{};
      return true;
    }
    if (c == 't' && static_cast<size_t>(end_ - p_) >= 4 &&
        std::string_view(p_, 4) == "true") {
      p_ += 4;
      out = true;
      return true;
    }
    if (c == 'f' && static_cast<size_t>(end_ - p_) >= 5 &&
        std::string_view(p_, 5) == "false") {
      p_ += 5;
      out = false;
      return true;
    }
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      return parseNumber(out);
    }
    return false;
  }
};

} // namespace

std::string metaToJsonString(const Meta &m, int indent) {
  if (indent < 0) {
    std::string o;
    o.push_back('{');
    bool first = true;
    for (const auto &[k, v] : m.entries()) {
      if (!first) {
        o.push_back(',');
      }
      first = false;
      appendEscapedJsonString(o, k);
      o.push_back(':');
      o += encodeJsonValue(v);
    }
    o.push_back('}');
    return o;
  }
  std::string o;
  metaObjectToStringPretty(o, m, indent, 0);
  return o;
}

bool metaFromJsonString(Meta &out, const std::string &json) {
  Parser p(json);
  return p.parseMeta(out);
}

} // namespace pp::common::io
