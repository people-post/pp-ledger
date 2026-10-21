#include "http_chain.h"

#include "ledger/chain/Types.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include "common/io/Json.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

using pp::common::Object;
using pp::common::Value;

namespace {

// 64-bit amounts and account ids do not survive a JSON number: decimal strings.
std::string dec(uint64_t v) { return std::to_string(v); }
std::string dec(int64_t v) { return std::to_string(v); }

constexpr uint64_t kDefaultBlocks = 20;
constexpr uint64_t kMaxBlocks = 100;

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

std::optional<uint64_t> parseU64(const std::string &s) {
  uint64_t v = 0;
  auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
  if (ec != std::errc() || end != s.data() + s.size() || s.empty()) {
    return std::nullopt;
  }
  return v;
}

std::string hex(const std::string &raw) { return pp::utl::hexEncode(raw); }

/** The node's state as a service follows it. */
Object statusJson(const pp::Client::BeaconState &state) {
  Object o;
  o.set("networkId", state.networkId);
  o.set("nextBlockId", dec(state.nextBlockId));
  o.set("finalizedHeight", state.finalizedHeight ? Value(dec(*state.finalizedHeight)) : Value(pp::common::Null{}));
  o.set("finalitySeconds", dec(state.finalitySeconds));
  if (state.signedTip) {
    o.set("tipHeight", dec(state.signedTip->height));
    o.set("generation", dec(state.signedTip->generation));
  }
  // A later generation certified (a takeover under way: blocks above its cutover go) — or not.
  if (state.certificate) {
    if (auto tx = pp::utl::binaryUnpack<pp::Ledger::TxBeaconCert>(state.certificate->data)) {
      pp::BeaconCertificate cert;
      if (cert.ltsFromString(tx.value().meta)) {
        Object c;
        c.set("generation", dec(cert.generation));
        c.set("cutoverHeight", cert.hasCutover() ? Value(dec(cert.cutoverHeight)) : Value(pp::common::Null{}));
        o.set("certificate", c);
      }
    }
  }
  // The beacon key signed two blocks at one height: the node halts above it.
  if (state.equivocation) {
    Object e;
    e.set("generation", dec(state.equivocation->generation()));
    e.set("height", dec(state.equivocation->height()));
    o.set("equivocation", e);
  }
  return o;
}

/**
 * A block as what moves balances: transfers (T_DEFAULT, and the native funding of a new account,
 * T_NEW_USER) and accounts ended (T_END_USER: their balances go to the recycle account). Renewals
 * and fees move only the native token's balance by fees; other records move no balance.
 */
Object blockJson(const pp::Ledger::ChainNode &node) {
  const auto &b = node.block;
  Object o;
  o.set("height", dec(b.index));
  o.set("hash", hex(node.hash));
  o.set("previousHash", hex(b.previousHash));
  o.set("slot", dec(b.slot));
  o.set("timestamp", dec(b.timestamp));
  o.set("finalAt", dec(b.finalAt));
  std::vector<Value> transfers;
  std::vector<Value> ended;
  for (size_t i = 0; i < b.records.size(); ++i) {
    const auto &record = b.records[i];
    auto transfer = [&](const char *type, uint64_t tokenId, uint64_t from, uint64_t to, uint64_t amount,
                        uint64_t fee) {
      Object t;
      t.set("index", dec(static_cast<uint64_t>(i)));
      t.set("type", std::string(type));
      t.set("tokenId", dec(tokenId));
      t.set("from", dec(from));
      t.set("to", dec(to));
      t.set("amount", dec(amount));
      t.set("fee", dec(fee));
      transfers.emplace_back(std::make_shared<Object>(t));
    };
    if (record.type == pp::Ledger::T_DEFAULT) {
      if (auto tx = pp::utl::binaryUnpack<pp::Ledger::TxDefault>(record.data)) {
        const auto &d = tx.value();
        transfer("transfer", d.tokenId, d.fromWalletId, d.toWalletId, d.amount, d.fee);
      }
    } else if (record.type == pp::Ledger::T_NEW_USER) {
      if (auto tx = pp::utl::binaryUnpack<pp::Ledger::TxNewUser>(record.data)) {
        const auto &d = tx.value();
        transfer("new-account", 0, d.fromWalletId, d.toWalletId, d.amount, d.fee);
      }
    } else if (record.type == pp::Ledger::T_END_USER) {
      if (auto tx = pp::utl::binaryUnpack<pp::Ledger::TxEndUser>(record.data)) {
        Object e;
        e.set("index", dec(static_cast<uint64_t>(i)));
        e.set("account", dec(tx.value().walletId));
        ended.emplace_back(std::make_shared<Object>(e));
      }
    }
  }
  o.set("transfers", pp::common::makeArray(std::move(transfers)));
  o.set("ended", pp::common::makeArray(std::move(ended)));
  return o;
}

} // namespace

void registerChainRoutes(httplib::Server &svr, pp::Client &node) {
  // The routes run on the server's worker threads; the client takes one call at a time.
  static std::mutex calls;
  svr.Get("/api/chain/status", [&node](const httplib::Request &, httplib::Response &res) {
    std::lock_guard lock(calls);
    auto state = node.fetchBeaconState();
    if (!state) {
      return sendError(res, 502, state.error().message);
    }
    sendJson(res, statusJson(state.value()));
  });

  // Blocks [from, from + limit) the node holds, oldest first, with its state when asked.
  svr.Get("/api/chain/blocks", [&node](const httplib::Request &req, httplib::Response &res) {
    auto from = parseU64(req.get_param_value("from"));
    const std::string limitParam = req.get_param_value("limit");
    auto limit = limitParam.empty() ? std::optional<uint64_t>(kDefaultBlocks) : parseU64(limitParam);
    if (!from || !limit || *limit == 0 || *limit > kMaxBlocks) {
      return sendError(res, 400, "from: a height; limit: 1 to " + std::to_string(kMaxBlocks));
    }
    std::lock_guard lock(calls);
    auto state = node.fetchBeaconState();
    if (!state) {
      return sendError(res, 502, state.error().message);
    }
    const uint64_t next = state.value().nextBlockId;
    const uint64_t end = *from >= next ? *from : std::min(next, *from + *limit);
    std::vector<Value> blocks;
    for (uint64_t height = *from; height < end; ++height) {
      auto block = node.fetchBlock(height);
      if (!block) {
        return sendError(res, 502, "Block " + std::to_string(height) + ": " + block.error().message);
      }
      blocks.emplace_back(std::make_shared<Object>(blockJson(block.value())));
    }
    Object o;
    o.set("status", statusJson(state.value()));
    o.set("blocks", pp::common::makeArray(std::move(blocks)));
    sendJson(res, o);
  });

  // An account's balances in the node's tip state.
  svr.Get(R"(/api/chain/accounts/(\d+))", [&node](const httplib::Request &req, httplib::Response &res) {
    auto id = parseU64(req.matches[1]);
    if (!id) {
      return sendError(res, 400, "Not an account id");
    }
    std::lock_guard lock(calls);
    auto account = node.fetchUserAccount(*id);
    if (!account) {
      return sendError(res, 404, account.error().message);
    }
    Object balances;
    for (const auto &[token, amount] : account.value().wallet.mBalances) {
      balances.set(dec(token), dec(amount));
    }
    Object o;
    o.set("id", dec(*id));
    o.set("balances", balances);
    sendJson(res, o);
  });
}
