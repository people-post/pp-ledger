#include "http_gateway.h"

#include "ledger/lib/common/Utilities.h"
#include "common/io/Json.h"

#include <charconv>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

using pp::common::Object;
using pp::common::Value;
using pp::gateway::At;
using pp::gateway::Gateway;

namespace {

// 64-bit amounts and account ids do not survive a JSON number: decimal strings.
std::string dec(uint64_t v) { return std::to_string(v); }
std::string dec(int64_t v) { return std::to_string(v); }

void sendJson(httplib::Response &res, const Object &o, int status = 200) {
  auto json = pp::common::io::valueToJsonString(Value(std::make_shared<Object>(o)), -1);
  res.status = status;
  res.set_content(json ? json.value() : std::string("{\"error\":\"unprintable\"}"), "application/json");
}

void sendError(httplib::Response &res, int status, const std::string &message) {
  Object o;
  o.set("error", message);
  sendJson(res, o, status);
}

void sendError(httplib::Response &res, const Gateway::Error &error) {
  sendError(res, error.code >= 400 && error.code < 600 ? error.code : 500, error.message);
}

std::optional<uint64_t> parseU64(const std::string &s) {
  uint64_t v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || s.empty()) {
    return std::nullopt;
  }
  return v;
}

/** `final`, `tip` (or nothing), a height, or `time:<unix seconds>`. */
std::optional<At> parseAt(const std::string &s) {
  if (s.empty() || s == "tip") {
    return At{At::Tip, 0};
  }
  if (s == "final") {
    return At{At::Final, 0};
  }
  const std::string timePrefix = "time:";
  const bool isTime = s.rfind(timePrefix, 0) == 0;
  auto v = parseU64(isTime ? s.substr(timePrefix.size()) : s);
  if (!v || *v > static_cast<uint64_t>(INT64_MAX)) {
    return std::nullopt;
  }
  return At{isTime ? At::Time : At::Height, static_cast<int64_t>(*v)};
}

Object blockJson(const pp::gateway::BlockView &b) {
  Object o;
  o.set("height", b.height);
  o.set("hash", pp::utl::hexEncode(b.hash));
  o.set("slot", b.slot);
  o.set("slotTime", b.slotTime);
  o.set("finalityTime", b.finalityTime);
  o.set("final", b.final);
  return o;
}

Object eventJson(const pp::gateway::Event &e) {
  using pp::gateway::Event;
  Object o;
  o.set("seq", e.seq);
  o.set("kind", std::string(Event::kindName(e.kind)));
  switch (e.kind) {
  case Event::Certificate:
    o.set("generation", e.generation);
    if (e.height != UINT64_MAX) {
      o.set("cutoverHeight", e.height);
    }
    break;
  case Event::Rewind:
    o.set("generation", e.generation);
    o.set("keptUpTo", e.height);
    o.set("droppedUpTo", e.droppedUpTo);
    o.set("crossedFinality", e.crossedFinality);
    break;
  case Event::Equivocation:
  case Event::Forked:
    o.set("generation", e.generation);
    o.set("height", e.height);
    break;
  case Event::Unexplained:
    o.set("accountId", dec(e.accountId));
    o.set("unknownFrom", e.droppedUpTo);
    o.set("height", e.height);
    break;
  default:
    break;
  }
  return o;
}

/** Constant time: the token is a secret. */
bool sameToken(const std::string &given, const std::string &token) {
  unsigned char diff = given.size() == token.size() ? 0 : 1;
  for (size_t i = 0; i < given.size(); ++i) {
    diff |= static_cast<unsigned char>(given[i] ^ token[i % token.size()]);
  }
  return diff == 0;
}

} // namespace

void registerGatewayRoutes(httplib::Server &svr, Gateway &gateway, const std::string &token) {
  // Wraps each handler: the bearer token first, when one is set.
  auto guarded = [token](auto handler) {
    return [token, handler](const httplib::Request &req, httplib::Response &res) {
      if (!token.empty()) {
        const std::string auth = req.get_header_value("Authorization");
        const std::string prefix = "Bearer ";
        if (auth.rfind(prefix, 0) != 0 || !sameToken(auth.substr(prefix.size()), token)) {
          sendError(res, 401, "A bearer token is required");
          return;
        }
      }
      handler(req, res);
    };
  };

  svr.Get("/v1/tip", guarded([&gateway](const httplib::Request &, httplib::Response &res) {
            auto tip = gateway.tip();
            if (!tip) {
              return sendError(res, tip.error());
            }
            const auto &t = tip.value();
            Object o = blockJson(t.block);
            o.set("generation", t.generation);
            if (t.finalizedHeight) {
              o.set("finalizedHeight", *t.finalizedHeight);
            } else {
              o.set("finalizedHeight", pp::common::Null{});
            }
            o.set("finalitySeconds", t.finalitySeconds);
            if (!t.halted.empty()) {
              o.set("halted", t.halted);
            }
            if (!t.finalityHeld.empty()) {
              o.set("finalityHeld", t.finalityHeld);
            }
            sendJson(res, o);
          }));

  svr.Get(R"(/v1/blocks/(\d+))", guarded([&gateway](const httplib::Request &req, httplib::Response &res) {
            auto height = parseU64(req.matches[1]);
            if (!height) {
              return sendError(res, 400, "Not a height");
            }
            auto block = gateway.blockAt(*height);
            if (!block) {
              return sendError(res, block.error());
            }
            sendJson(res, blockJson(block.value()));
          }));

  svr.Get("/v1/watch", guarded([&gateway](const httplib::Request &, httplib::Response &res) {
            std::vector<Value> ids;
            for (uint64_t id : gateway.watched()) {
              ids.emplace_back(dec(id));
            }
            Object o;
            o.set("accounts", pp::common::makeArray(std::move(ids)));
            sendJson(res, o);
          }));

  svr.Put(R"(/v1/watch/(\d+))", guarded([&gateway](const httplib::Request &req, httplib::Response &res) {
            auto id = parseU64(req.matches[1]);
            if (!id) {
              return sendError(res, 400, "Not an account id");
            }
            if (auto watched = gateway.watch(*id); !watched) {
              return sendError(res, watched.error());
            }
            Object o;
            o.set("accountId", dec(*id));
            o.set("watched", true);
            sendJson(res, o);
          }));

  svr.Delete(R"(/v1/watch/(\d+))", guarded([&gateway](const httplib::Request &req, httplib::Response &res) {
               auto id = parseU64(req.matches[1]);
               if (!id) {
                 return sendError(res, 400, "Not an account id");
               }
               if (auto unwatched = gateway.unwatch(*id); !unwatched) {
                 return sendError(res, unwatched.error());
               }
               Object o;
               o.set("accountId", dec(*id));
               o.set("watched", false);
               sendJson(res, o);
             }));

  svr.Get(R"(/v1/accounts/(\d+)/balances)",
          guarded([&gateway](const httplib::Request &req, httplib::Response &res) {
            auto id = parseU64(req.matches[1]);
            if (!id) {
              return sendError(res, 400, "Not an account id");
            }
            auto at = parseAt(req.get_param_value("at"));
            if (!at) {
              return sendError(res, 400, "at: final, tip, a height, or time:<unix seconds>");
            }
            auto balances = gateway.balances(*id, *at);
            if (!balances) {
              return sendError(res, balances.error());
            }
            const auto &b = balances.value();
            Object amounts;
            for (const auto &[tokenId, amount] : b.balances) {
              amounts.set(dec(tokenId), dec(amount));
            }
            Object o;
            o.set("accountId", dec(b.accountId));
            o.set("height", b.height);
            o.set("present", b.present);
            o.set("balances", amounts);
            sendJson(res, o);
          }));

  svr.Get("/v1/events", guarded([&gateway](const httplib::Request &req, httplib::Response &res) {
            const std::string after = req.get_param_value("after");
            auto from = after.empty() ? std::optional<uint64_t>(0) : parseU64(after);
            const std::string limitParam = req.get_param_value("limit");
            auto limit = limitParam.empty() ? std::optional<uint64_t>(100) : parseU64(limitParam);
            if (!from || !limit || *limit == 0 || *limit > 1000) {
              return sendError(res, 400, "after: a sequence number; limit: 1 to 1000");
            }
            std::vector<Value> list;
            for (const auto &event : gateway.events(*from, *limit)) {
              list.emplace_back(std::make_shared<Object>(eventJson(event)));
            }
            Object o;
            o.set("events", pp::common::makeArray(std::move(list)));
            sendJson(res, o);
          }));
}
