/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 * Also exposes a Model Context Protocol (MCP) server via SSE transport.
 */
#include "Client.h"
#include "AccountAttachment.h"
#include "AccountIds.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "common/io/Json.h"
#include "lib/http/httplib.h"

#include <cli11.hpp>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include <sodium.h>

using pp::common::Array;
using pp::common::ArrayPtr;
using pp::common::Null;
using pp::common::Object;
using pp::common::ObjectPtr;
using pp::common::Value;
using pp::common::asArray;
using pp::common::asNonNegInt;
using pp::common::asObject;
using pp::common::asString;
using pp::common::isNullValue;
using pp::common::io::metaToJsonString;
using pp::common::io::valueFromJsonString;
using pp::common::io::valueToJsonString;

static constexpr uint64_t ID_GENESIS = pp::AccountIds::ID_GENESIS;
static constexpr uint64_t ID_FIRST_USER = pp::AccountIds::ID_FIRST_USER;
static constexpr size_t MAX_MCP_SESSIONS = 4;
static constexpr size_t MAX_MCP_PENDING_EVENTS_PER_SESSION = 256;
static constexpr size_t HTTP_PAYLOAD_MAX_LENGTH = 2 * 1024 * 1024; // 2 MiB

static uint64_t randomAccountId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  // Cap at INT64_MAX so auto-generated ids always encode as JSON numbers.
  std::uniform_int_distribution<uint64_t> dist(
      ID_FIRST_USER, static_cast<uint64_t>(std::numeric_limits<int64_t>::max()));
  return dist(gen);
}

static void setValidationWindow(uint64_t& idempotentId, int64_t& validationTsMin,
                                int64_t& validationTsMax) {
  const int64_t now = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  idempotentId = static_cast<uint64_t>(now) ^ (randomAccountId() & 0xFFFFULL);
  if (idempotentId == 0) idempotentId = 1;
  validationTsMin = now - 60;
  validationTsMax = now + 3600;
}

// ── MCP: session state ──────────────────────────────────────────────────────

struct McpSession {
  std::mutex              mutex;
  std::condition_variable cv;
  std::queue<std::string> pending; // pre-formatted SSE strings
  const size_t            maxPendingEvents;
  bool                    closed{false};

  explicit McpSession(size_t maxPendingEventsIn)
      : maxPendingEvents(maxPendingEventsIn) {}

  bool enqueue(std::string event) {
    std::lock_guard<std::mutex> lk(mutex);
    if (closed) return false;
    if (pending.size() >= maxPendingEvents) {
      // Drop the oldest event to keep bounded memory for slow clients.
      pending.pop();
    }
    pending.push(std::move(event));
    cv.notify_one();
    return true;
  }

  void close() {
    std::lock_guard<std::mutex> lk(mutex);
    closed = true;
    cv.notify_all();
  }
};

// ── MCP: helpers ────────────────────────────────────────────────────────────

static std::string generateSessionId() {
  uint8_t buf[16];
  randombytes_buf(buf, sizeof(buf));
  char hex[33];
  sodium_bin2hex(hex, sizeof(hex), buf, sizeof(buf));
  return std::string(hex);
}

static std::string makeSseEvent(const std::string& type, const std::string& data) {
  return "event: " + type + "\ndata: " + data + "\n\n";
}


static std::string dumpJson(const Value &v, int indent = -1) {
  auto r = valueToJsonString(v, indent);
  if (!r.isOk()) {
    return std::string("{\"error\":\"") + r.error().message + "\"}";
  }
  return r.value();
}

static std::string dumpJson(const Object &o, int indent = -1) {
  return dumpJson(Value(std::make_shared<Object>(o)), indent);
}

static Value parseJsonOrEmpty(const std::string &s, std::string *err = nullptr) {
  auto r = valueFromJsonString(s);
  if (!r.isOk()) {
    if (err) *err = r.error().message;
    return Null{};
  }
  return std::move(r.value());
}

static uint64_t valueToUint64(const Value &v, uint64_t defaultVal) {
  if (auto n = asNonNegInt(v)) return *n;
  return defaultVal;
}

static uint64_t objectUint64(const Object &o, const std::string &key, uint64_t defaultVal) {
  auto slot = o.fields().tryGet(key);
  if (!slot) return defaultVal;
  return valueToUint64(slot->get(), defaultVal);
}

static std::string objectString(const Object &o, const std::string &key,
                                const std::string &defaultVal = {}) {
  auto s = o.getString(key);
  return s ? *s : defaultVal;
}

static bool parseRequestObject(const std::string &bodyText, Object &out,
                               std::string &errOut) {
  Value bodyVal = parseJsonOrEmpty(bodyText, &errOut);
  Object *bodyPtr = asObject(bodyVal);
  if (!bodyPtr) {
    if (errOut.empty()) errOut = "Invalid JSON in request body";
    return false;
  }
  out = *bodyPtr;
  return true;
}


static Object makeRpcResult(const Value &id, const Object &result) {
  Object o;
  o.set("jsonrpc", std::string("2.0"));
  o.set("id", id);
  o.set("result", result);
  return o;
}

static Object makeRpcError(const Value &id, int code, const std::string &message) {
  Object err;
  err.set("code", static_cast<int64_t>(code));
  err.set("message", message);
  Object o;
  o.set("jsonrpc", std::string("2.0"));
  o.set("id", id);
  o.set("error", err);
  return o;
}

struct McpTool {
  std::string name;
  std::string description;
  Object inputSchema;
  std::function<Object(const Object&, pp::Client&, pp::Client&)> handler;
};

struct McpResource {
  std::string uri;
  std::string name;
  std::string description;
  std::string mimeType;
  std::function<Object(pp::Client&, pp::Client&)> handler;
};

static std::vector<McpTool> g_mcpTools;
static std::vector<McpResource> g_mcpResources;

static void registerMcpTool(McpTool tool) {
  g_mcpTools.push_back(std::move(tool));
}

static void registerMcpResource(McpResource resource) {
  g_mcpResources.push_back(std::move(resource));
}

static Object mcpOk(const std::string &text) {
  Object contentItem;
  contentItem.set("type", std::string("text"));
  contentItem.set("text", text);
  Object o;
  o.set("content", Object::array({Value(std::make_shared<Object>(contentItem))}));
  o.set("isError", false);
  return o;
}

static Object mcpErr(const std::string &text) {
  Object contentItem;
  contentItem.set("type", std::string("text"));
  contentItem.set("text", text);
  Object o;
  o.set("content", Object::array({Value(std::make_shared<Object>(contentItem))}));
  o.set("isError", true);
  return o;
}

static Object emptySchema() {
  Object o;
  o.set("type", std::string("object"));
  o.set("properties", Object{});
  o.set("required", Object::array({}));
  return o;
}

// ── MCP: JSON-RPC dispatcher ────────────────────────────────────────────────

static std::optional<Object> handleMcpRpc(const Object &req,
                                         pp::Client &beaconClient, pp::Client &minerClient) {
  auto jsonrpc = req.getString("jsonrpc");
  if (!jsonrpc || *jsonrpc != "2.0" || !req.contains("method"))
    return makeRpcError(Null{}, -32600, "Invalid Request");

  const std::string method = *req.getString("method");
  const bool isNotification = !req.contains("id");
  Value id = Null{};
  if (!isNotification) {
    auto slot = req.fields().tryGet("id");
    if (slot) id = slot->get();
  }
  Object params;
  if (const Object *p = req.getObject("params")) {
    params = *p;
  }

  if (isNotification) return std::nullopt;

  if (method == "initialize") {
    Object caps;
    caps.set("tools", Object{});
    caps.set("resources", Object{});
    Object serverInfo;
    serverInfo.set("name", std::string("pp-ledger-mcp"));
    serverInfo.set("version", std::string("1.0.0"));
    Object result;
    result.set("protocolVersion", std::string("2024-11-05"));
    result.set("capabilities", caps);
    result.set("serverInfo", serverInfo);
    return makeRpcResult(id, result);
  }
  if (method == "ping") {
    return makeRpcResult(id, Object{});
  }
  if (method == "tools/list") {
    std::vector<Value> tools;
    for (const auto &t : g_mcpTools) {
      Object tool;
      tool.set("name", t.name);
      tool.set("description", t.description);
      tool.set("inputSchema", t.inputSchema);
      tools.push_back(std::make_shared<Object>(tool));
    }
    Object result;
    result.set("tools", Object::array(std::move(tools)));
    return makeRpcResult(id, result);
  }
  if (method == "tools/call") {
    const std::string name = objectString(params, "name");
    Object args;
    if (const Object *a = params.getObject("arguments")) {
      args = *a;
    }
    for (const auto &t : g_mcpTools) {
      if (t.name == name)
        return makeRpcResult(id, t.handler(args, beaconClient, minerClient));
    }
    return makeRpcResult(id, mcpErr("Unknown tool: " + name));
  }
  if (method == "resources/list") {
    std::vector<Value> resources;
    for (const auto &r : g_mcpResources) {
      Object resource;
      resource.set("uri", r.uri);
      resource.set("name", r.name);
      resource.set("description", r.description);
      resource.set("mimeType", r.mimeType);
      resources.push_back(std::make_shared<Object>(resource));
    }
    Object result;
    result.set("resources", Object::array(std::move(resources)));
    return makeRpcResult(id, result);
  }
  if (method == "resources/read") {
    const std::string uri = objectString(params, "uri");
    for (const auto &r : g_mcpResources) {
      if (r.uri == uri) {
        Object result = r.handler(beaconClient, minerClient);
        if (result.contains("error")) {
          auto err = result.getString("error");
          return makeRpcError(id, -32602, err ? *err : "error");
        }
        return makeRpcResult(id, result);
      }
    }
    return makeRpcError(id, -32602, "Unknown resource: " + uri);
  }

  return makeRpcError(id, -32601, "Method not found: " + method);
}

// ── existing helpers ────────────────────────────────────────────────────────

static void parseEndpoint(const std::string& spec, std::string& host, uint16_t& port,
                          const std::string& defaultHost, uint16_t defaultPort) {
  host = defaultHost;
  port = defaultPort;
  uint16_t extracted = 0;
  if (pp::utl::parseHostPort(spec, host, extracted)) {
    if (extracted != 0)
      port = extracted;
  }
}

static void setJsonError(httplib::Response& res, int status, const std::string& message) {
  res.status = status;
  Object o;
  o.set("error", message);
  res.set_content(dumpJson(o), "application/json");
}

static std::string htmlEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c; break;
    }
  }
  return out;
}

static std::string makeErrorHtml(int status, const std::string& path,
                                 const std::string& title, const std::string& message) {
  std::string pathEsc = htmlEscape(path);
  return R"(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>)" + std::to_string(status) + " " + htmlEscape(title) + R"(</title>
  <style>
    * { box-sizing: border-box; margin: 0; padding: 0; }
    body {
      font-family: 'Segoe UI', system-ui, -apple-system, sans-serif;
      background: linear-gradient(135deg, #1a1a2e 0%, #16213e 50%, #0f3460 100%);
      min-height: 100vh;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #e8e8e8;
      padding: 1.5rem;
    }
    .card {
      background: rgba(255,255,255,0.06);
      border-radius: 16px;
      padding: 2.5rem;
      max-width: 480px;
      text-align: center;
      border: 1px solid rgba(255,255,255,0.1);
      box-shadow: 0 8px 32px rgba(0,0,0,0.3);
    }
    .status { font-size: 4rem; font-weight: 700; color: #e94560; margin-bottom: 0.5rem; }
    h1 { font-size: 1.25rem; font-weight: 600; margin-bottom: 1rem; color: #fff; }
    p { color: #b8b8b8; line-height: 1.6; margin-bottom: 1rem; }
    .path { font-family: monospace; background: rgba(0,0,0,0.3); padding: 0.5rem 0.75rem; border-radius: 8px; word-break: break-all; margin: 1rem 0; font-size: 0.9rem; }
    .hint { font-size: 0.875rem; color: #7a8a9a; margin-top: 1.5rem; }
    a { color: #e94560; text-decoration: none; }
    a:hover { text-decoration: underline; }
  </style>
</head>
<body>
  <div class="card">
    <div class="status">)" + std::to_string(status) + R"(</div>
    <h1>)" + htmlEscape(title) + R"(</h1>
    <p>)" + htmlEscape(message) + R"(</p>
    <div class="path">)" + (pathEsc.empty() ? "/" : pathEsc) + R"(</div>
    <p class="hint">Try <a href="/api/beacon/state">/api/beacon/state</a> or <a href="/api/miner/status">/api/miner/status</a> for the API.</p>
  </div>
</body>
</html>)";
}

// ── API route handlers ───────────────────────────────────────────────────────

static void handleBeaconState(const httplib::Request&, httplib::Response& res,
                              pp::Client& beaconClient) {
  auto r = beaconClient.fetchBeaconState();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleBeaconCalibration(const httplib::Request&, httplib::Response& res,
                                   pp::Client& beaconClient) {
  auto r = beaconClient.fetchCalibration();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleBeaconMiners(const httplib::Request&, httplib::Response& res,
                               pp::Client& beaconClient) {
  auto r = beaconClient.fetchMinerList();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  std::vector<Value> elems;
  for (const auto &m : r.value()) {
    elems.push_back(std::make_shared<Object>(m.ltsToMeta()));
  }
  res.set_content(dumpJson(Object::array(std::move(elems))), "application/json");
}

static void handleMinerStatus(const httplib::Request&, httplib::Response& res,
                             pp::Client& minerClient) {
  auto r = minerClient.fetchMinerStatus();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleBlockGet(const httplib::Request& req, httplib::Response& res,
                          pp::Client& beaconClient) {
  uint64_t blockId = std::stoull(req.matches[1].str());
  auto r = beaconClient.fetchBlock(blockId);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleAccountGet(const httplib::Request& req, httplib::Response& res,
                            pp::Client& beaconClient) {
  uint64_t accountId = std::stoull(req.matches[1].str());
  auto r = beaconClient.fetchUserAccount(accountId);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleAccountCreate(const httplib::Request& req, httplib::Response& res,
                                pp::Client& minerClient) {
  std::string parseErr;
  Value bodyVal = parseJsonOrEmpty(req.body, &parseErr);
  Object *bodyPtr = asObject(bodyVal);
  if (!bodyPtr) {
    setJsonError(res, 400, parseErr.empty() ? "Invalid JSON in request body" : parseErr);
    return;
  }
  const Object &body = *bodyPtr;
  if (!body.contains("from") || !body.contains("amount") || !body.contains("key")) {
    setJsonError(res, 400, "from, amount, and key are required");
    return;
  }
  uint64_t fromWalletId = objectUint64(body, "from", 0);
  uint64_t amount = objectUint64(body, "amount", 0);
  uint64_t toWalletId = objectUint64(body, "to", 0);
  if (toWalletId == 0) toWalletId = randomAccountId();
  uint64_t fee = objectUint64(body, "fee", 0);
  std::string newPubkeyHex = objectString(body, "newPubkey");
  std::string metaDesc = objectString(body, "meta");
  uint8_t minSignatures = static_cast<uint8_t>(objectUint64(body, "minSignatures", 1));

  std::string pubkeyToUse;
  std::string privateKeyToPrint;
  if (!newPubkeyHex.empty()) {
    std::string pk = newPubkeyHex;
    if (pk.size() >= 2 && (pk[0] == '0' && (pk[1] == 'x' || pk[1] == 'X')))
      pk = pk.substr(2);
    std::string decoded = pp::utl::hexDecode(pk);
    if (decoded.size() != pp::utl::kMlDsaPublicKeyBytes) {
      setJsonError(res, 400, "newPubkey must be ML-DSA-65 public key (1952 bytes / 3904 hex chars)");
      return;
    }
    pubkeyToUse = decoded;
  } else {
    auto pair = pp::utl::mlDsaGenerate();
    if (!pair.isOk()) {
      setJsonError(res, 500, std::string("Key generation failed: ") + pair.error().message);
      return;
    }
    pubkeyToUse = pair->publicKey;
    privateKeyToPrint = pp::utl::hexEncode(pair->privateKey);
  }

  pp::Client::UserAccount userAccount;
  userAccount.wallet.publicKeys.push_back(pubkeyToUse);
  userAccount.wallet.minSignatures = minSignatures;
  userAccount.wallet.keyType = pp::Crypto::TK_ML_DSA_65;
  userAccount.wallet.mBalances[ID_GENESIS] = static_cast<int64_t>(amount);
  (void)metaDesc;
  userAccount.meta = pp::AccountAttachment::emptySerialized();

  std::string keyStr = pp::utl::readKey(objectString(body, "key"));
  if (keyStr.size() >= 2 && (keyStr[0] == '0' && (keyStr[1] == 'x' || keyStr[1] == 'X')))
    keyStr = keyStr.substr(2);
  std::string privateKey = pp::utl::hexDecode(keyStr);
  if (privateKey.size() != pp::utl::kMlDsaPrivateKeyBytes) {
    setJsonError(res, 400, "key must be ML-DSA-65 private key (4032 bytes / 8064 hex chars)");
    return;
  }

  pp::Ledger::TxNewUser tx;
  tx.fromWalletId = fromWalletId;
  tx.toWalletId = toWalletId;
  tx.amount = amount;
  tx.fee = fee;
  tx.meta = userAccount.ltsToString();
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  std::string payload = pp::utl::binaryPack(tx);
  pp::Ledger::Record rec;
  rec.type = pp::Ledger::T_NEW_USER;
  rec.data = std::move(payload);
  std::string networkId;
  if (auto st = minerClient.fetchBeaconState()) {
    networkId = st->networkId;
  }
  auto sigResult =
      pp::utl::mlDsaSign(privateKey, rec.signingMessage(networkId));
  if (!sigResult) {
    setJsonError(res, 500, std::string("Sign failed: ") + sigResult.error().message);
    return;
  }
  rec.signatures = {*sigResult};

  auto r = minerClient.addTransaction(rec);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }

  Object resp;
  resp.setUIntForJson("newAccountId", toWalletId);
  if (!privateKeyToPrint.empty()) {
    resp.set("publicKey", pp::utl::hexEncode(pubkeyToUse));
    resp.set("privateKey", privateKeyToPrint);
  }
  res.status = 201;
  res.set_content(dumpJson(resp), "application/json");
}

static void handleTxByWallet(const httplib::Request& req, httplib::Response& res,
                            pp::Client& beaconClient) {
  pp::Client::TxGetByWalletRequest wr;
  if (req.has_param("walletId")) {
    try {
      wr.walletId = std::stoull(req.get_param_value("walletId"));
    } catch (...) {
      setJsonError(res, 400, "Invalid walletId");
      return;
    }
  }
  if (req.has_param("beforeBlockId")) {
    try {
      wr.beforeBlockId = std::stoull(req.get_param_value("beforeBlockId"));
    } catch (...) {
      setJsonError(res, 400, "Invalid beforeBlockId");
      return;
    }
  }
  auto r = beaconClient.fetchTransactionsByWallet(wr);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static void handleTxByIndex(const httplib::Request& req, httplib::Response& res,
                            pp::Client& beaconClient) {
  if (!req.has_param("txIndex")) {
    setJsonError(res, 400, "txIndex is required");
    return;
  }
  uint64_t txIndex;
  try {
    txIndex = std::stoull(req.get_param_value("txIndex"));
  } catch (...) {
    setJsonError(res, 400, "Invalid txIndex");
    return;
  }
  pp::Client::TxGetByIndexRequest wr;
  wr.txIndex = txIndex;
  auto r = beaconClient.fetchTransactionByIndex(wr);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(pp::common::io::metaToJsonString(r.value().ltsToMeta()),
                  "application/json");
}

static bool isHexStringStrict(const std::string& input) {
  if (input.empty() || (input.size() % 2) != 0) return false;
  for (unsigned char ch : input) {
    if (!std::isxdigit(static_cast<unsigned char>(ch))) return false;
  }
  return true;
}

static void handleTxBuild(const httplib::Request& req, httplib::Response& res,
                          pp::Client& /*minerClient*/) {
  Object body;
  std::string parseErr;
  if (!parseRequestObject(req.body, body, parseErr)) {
    setJsonError(res, 400, parseErr);
    return;
  }

  const uint16_t type =
      static_cast<uint16_t>(objectUint64(body, "type", pp::Ledger::T_DEFAULT));

  uint64_t fromWalletId = 0;
  uint64_t toWalletId = 0;
  uint64_t walletId = 0;
  uint64_t tokenId = 0;
  uint64_t amount = 0;
  if (type == pp::Ledger::T_USER_UPDATE ||
      type == pp::Ledger::T_RENEWAL ||
      type == pp::Ledger::T_END_USER) {
    if (!body.contains("walletId")) {
      setJsonError(res, 400, "walletId is required for this transaction type");
      return;
    }
    walletId = objectUint64(body, "walletId", 0);
  } else if (type == pp::Ledger::T_GENESIS || type == pp::Ledger::T_CONFIG) {
    // Genesis-only transactions: no wallet identifiers in payload.
  } else {
    if (!body.contains("fromWalletId") || !body.contains("toWalletId") || !body.contains("amount")) {
      setJsonError(res, 400, "fromWalletId, toWalletId, and amount are required");
      return;
    }
    fromWalletId = objectUint64(body, "fromWalletId", 0);
    toWalletId = objectUint64(body, "toWalletId", 0);
    amount = objectUint64(body, "amount", 0);
    if (type == pp::Ledger::T_DEFAULT) {
      tokenId = objectUint64(body, "tokenId", 0);
    }
  }

  pp::Ledger::TxCommon common;
  common.fee = objectUint64(body, "fee", 0);

  if (auto metaHexOpt = body.getString("metaHex")) {
    std::string metaHex = *metaHexOpt;
    if (!metaHex.empty()) {
      if (!isHexStringStrict(metaHex)) {
        setJsonError(res, 400, "metaHex must be an even-length hex string without 0x prefix");
        return;
      }
      common.meta = pp::utl::hexDecode(metaHex);
    }
  }

  uint64_t idempotentId = 0;
  int64_t validationTsMin = 0;
  int64_t validationTsMax = 0;

  if (body.contains("idempotentId")) {
    idempotentId = objectUint64(body, "idempotentId", 0);
  }
  if (body.contains("validationTsMin")) {
    validationTsMin = static_cast<int64_t>(objectUint64(body, "validationTsMin", 0));
  }
  if (body.contains("validationTsMax")) {
    validationTsMax = static_cast<int64_t>(objectUint64(body, "validationTsMax", 0));
  }
  if (!body.contains("validationTsMin") && !body.contains("validationTsMax")) {
    // Only apply a default window for tx types that support it.
    setValidationWindow(idempotentId, validationTsMin, validationTsMax);
  }

  auto packTyped = [&](uint16_t t) -> std::string {
    auto fillCommon = [&](auto &x) {
      x.fee = common.fee;
      x.meta = common.meta;
    };
    auto fillIdempotency = [&](auto &x) {
      x.idempotentId = idempotentId;
      x.validationTsMin = validationTsMin;
      x.validationTsMax = validationTsMax;
    };
    switch (t) {
    case pp::Ledger::T_GENESIS: {
      pp::Ledger::TxGenesis x; fillCommon(x); return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_NEW_USER: {
      auto fill = [&](auto &x) {
        fillCommon(x);
        fillIdempotency(x);
        x.fromWalletId = fromWalletId;
        x.toWalletId = toWalletId;
        x.amount = amount;
      };
      pp::Ledger::TxNewUser x; fill(x); return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_CONFIG: {
      pp::Ledger::TxConfig x; fillCommon(x); fillIdempotency(x); return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_USER_UPDATE: {
      pp::Ledger::TxUserUpdate x; fillCommon(x); fillIdempotency(x); x.walletId = walletId; return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_RENEWAL: {
      pp::Ledger::TxRenewal x; fillCommon(x); x.walletId = walletId; return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_END_USER: {
      pp::Ledger::TxEndUser x; fillCommon(x); x.walletId = walletId; return pp::utl::binaryPack(x);
    }
    case pp::Ledger::T_DEFAULT:
    default: {
      auto fill = [&](auto &x) {
        fillCommon(x);
        fillIdempotency(x);
        x.tokenId = tokenId;
        x.fromWalletId = fromWalletId;
        x.toWalletId = toWalletId;
        x.amount = amount;
      };
      pp::Ledger::TxDefault x; fill(x); return pp::utl::binaryPack(x);
    }
    }
  };

  std::string unsignedTxPayload = packTyped(type);
  Object resp;
  resp.setJsonUInt("type", type);
  resp.set("transactionHex", pp::utl::hexEncode(unsignedTxPayload));
  res.set_content(dumpJson(resp), "application/json");
}

static void handleTxSubmit(const httplib::Request& req, httplib::Response& res,
                           pp::Client& minerClient) {
  Object body;
  std::string parseErr;
  if (!parseRequestObject(req.body, body, parseErr)) {
    setJsonError(res, 400, parseErr);
    return;
  }
  if (!body.contains("type")) {
    setJsonError(res, 400, "type is required");
    return;
  }
  auto txHexOpt = body.getString("transactionHex");
  if (!txHexOpt) {
    setJsonError(res, 400, "transactionHex is required and must be a string");
    return;
  }
  const Array *sigsArr = body.getArray("signaturesHex");
  if (!sigsArr) {
    setJsonError(res, 400, "signaturesHex is required and must be an array");
    return;
  }

  const uint16_t type = static_cast<uint16_t>(objectUint64(body, "type", pp::Ledger::T_DEFAULT));
  std::string transactionHex = *txHexOpt;
  if (!isHexStringStrict(transactionHex)) {
    setJsonError(res, 400, "transactionHex must be a non-empty even-length hex string without 0x prefix");
    return;
  }
  std::string transactionPayload = pp::utl::hexDecode(transactionHex);
  if (transactionPayload.empty()) {
    setJsonError(res, 400, "transactionHex failed to decode");
    return;
  }

  pp::Ledger::Record rec;
  rec.type = type;
  rec.data = std::move(transactionPayload);

  if (sigsArr->elements.empty()) {
    setJsonError(res, 400, "signaturesHex must contain at least one signature");
    return;
  }
  for (const auto& item : sigsArr->elements) {
    auto sigHexOpt = asString(item);
    if (!sigHexOpt) {
      setJsonError(res, 400, "signaturesHex entries must be strings");
      return;
    }
    std::string sigHex = *sigHexOpt;
    constexpr size_t kSigHexLen = pp::utl::kMlDsaSignatureBytes * 2;
    if (!isHexStringStrict(sigHex) || sigHex.size() != kSigHexLen) {
      setJsonError(res, 400, "each signature hex must be exactly " +
                   std::to_string(kSigHexLen) +
                   " hex chars (ML-DSA-65), without 0x prefix");
      return;
    }
    std::string sig = pp::utl::hexDecode(sigHex);
    if (sig.size() != pp::utl::kMlDsaSignatureBytes) {
      setJsonError(res, 400, "signature hex failed to decode to ML-DSA-65 size");
      return;
    }
    rec.signatures.push_back(std::move(sig));
  }

  auto r = minerClient.addTransaction(rec);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.status = 204;
}

// ── MCP route handlers ──────────────────────────────────────────────────────

static void handleMcpSse(const httplib::Request& req, httplib::Response& res,
                         const std::string& httpHost, uint16_t httpPort,
                         std::map<std::string, std::shared_ptr<McpSession>>& mcpSessions,
                         std::mutex& mcpSessionsMutex) {
  (void)req;
  std::string sessionId = generateSessionId();
  auto session = std::make_shared<McpSession>(MAX_MCP_PENDING_EVENTS_PER_SESSION);
  {
    std::lock_guard<std::mutex> lk(mcpSessionsMutex);
    if (mcpSessions.size() >= MAX_MCP_SESSIONS) {
      res.set_header("Retry-After", "5");
      setJsonError(res, 503, "Too many active MCP sessions");
      return;
    }
    mcpSessions[sessionId] = session;
  }

  std::string endpointUrl = "http://" + httpHost + ":" + std::to_string(httpPort) +
                            "/mcp/messages?sessionId=" + sessionId;
  session->enqueue(makeSseEvent("endpoint", endpointUrl));

  res.set_header("Cache-Control", "no-cache");
  res.set_header("Connection", "keep-alive");
  res.set_header("X-Accel-Buffering", "no");
  res.set_chunked_content_provider(
    "text/event-stream",
    [session](size_t /*offset*/, httplib::DataSink& sink) -> bool {
      std::string event;
      {
        std::unique_lock<std::mutex> lk(session->mutex);
        session->cv.wait_for(lk, std::chrono::seconds(15), [&session] {
          return !session->pending.empty() || session->closed;
        });
        if (session->closed && session->pending.empty()) return false;
        if (!session->pending.empty()) {
          event = std::move(session->pending.front());
          session->pending.pop();
        }
      }
      if (!event.empty()) return sink.write(event.c_str(), event.size());
      static const std::string ping = ": ping\n\n";
      return sink.write(ping.c_str(), ping.size());
    },
    [&mcpSessionsMutex, &mcpSessions, sessionId](bool /*success*/) {
      std::lock_guard<std::mutex> lk(mcpSessionsMutex);
      auto it = mcpSessions.find(sessionId);
      if (it != mcpSessions.end()) {
        it->second->close();
        mcpSessions.erase(it);
      }
    }
  );
}

static void handleMcpMessages(const httplib::Request& req, httplib::Response& res,
                              pp::Client& beaconClient, pp::Client& minerClient,
                              std::map<std::string, std::shared_ptr<McpSession>>& mcpSessions,
                              std::mutex& mcpSessionsMutex) {
  std::string sessionId = req.get_param_value("sessionId");
  std::shared_ptr<McpSession> session;
  {
    std::lock_guard<std::mutex> lk(mcpSessionsMutex);
    auto it = mcpSessions.find(sessionId);
    if (it == mcpSessions.end()) {
      setJsonError(res, 404, "Session not found: " + sessionId);
      return;
    }
    session = it->second;
  }

  std::string parseErr;
  Value bodyVal = parseJsonOrEmpty(req.body, &parseErr);
  if (isNullValue(bodyVal) && !parseErr.empty()) {
    setJsonError(res, 400, parseErr);
    return;
  }

  bool enqueueFailed = false;
  auto handle = [&](const Object& rpc) {
    if (enqueueFailed) return;
    auto response = handleMcpRpc(rpc, beaconClient, minerClient);
    if (response) {
      if (!session->enqueue(makeSseEvent("message", dumpJson(*response)))) {
        enqueueFailed = true;
      }
    }
  };

  if (const Array *arr = asArray(bodyVal)) {
    for (const auto& item : arr->elements) {
      const Object *rpc = asObject(item);
      if (!rpc) {
        setJsonError(res, 400, "MCP batch items must be JSON objects");
        return;
      }
      handle(*rpc);
    }
  } else if (const Object *obj = asObject(bodyVal)) {
    handle(*obj);
  } else {
    setJsonError(res, 400, parseErr.empty() ? "Invalid JSON in request body" : parseErr);
    return;
  }

  if (enqueueFailed) {
    setJsonError(res, 409, "Session is closed");
    return;
  }

  res.status = 202;
  res.set_content("", "application/json");
}

int main(int argc, char** argv) {
  CLI::App app{"HTTP API server for pp-ledger (client interfaces)"};
  uint16_t httpPort = 8080;
  std::string httpHost = "0.0.0.0";
  std::string beaconSpec = "localhost:8517";
  std::string minerSpec = "localhost:8518";
  app.add_option("--port", httpPort, "HTTP server port")->default_val(8080);
  app.add_option("--bind", httpHost, "HTTP bind address")->default_val("0.0.0.0");
  app.add_option("--beacon", beaconSpec, "Beacon endpoint (host:port)")->default_str("localhost:8517");
  app.add_option("--miner", minerSpec, "Miner endpoint (host:port)")->default_str("localhost:8518");
  CLI11_PARSE(app, argc, argv);

  std::string beaconHost;
  uint16_t beaconPort = pp::Client::DEFAULT_BEACON_PORT;
  parseEndpoint(beaconSpec, beaconHost, beaconPort, "localhost", pp::Client::DEFAULT_BEACON_PORT);
  std::string minerHost;
  uint16_t minerPort = pp::Client::DEFAULT_MINER_PORT;
  parseEndpoint(minerSpec, minerHost, minerPort, "localhost", pp::Client::DEFAULT_MINER_PORT);

  pp::Client beaconClient;
  beaconClient.setEndpoint(pp::network::IpEndpoint{beaconHost, beaconPort});
  pp::Client minerClient;
  minerClient.setEndpoint(pp::network::IpEndpoint{minerHost, minerPort});

  // MCP session registry
  std::map<std::string, std::shared_ptr<McpSession>> mcpSessions;
  std::mutex mcpSessionsMutex;

  httplib::Server svr;
  // API requests here are small JSON payloads; keep a tighter cap than httplib default.
  svr.set_payload_max_length(HTTP_PAYLOAD_MAX_LENGTH);
  auto httpLog = pp::logging::getLogger("HttpServer");
  svr.set_logger([&httpLog](const httplib::Request& req, const httplib::Response& res) {
    httpLog.info << req.method << " " << req.path << " " << res.status
                 << " (" << (req.remote_addr.empty() ? "-" : req.remote_addr) << ")";
  });
  svr.set_error_logger([&httpLog](const httplib::Error& err, const httplib::Request* req) {
    std::string path = req ? req->path : "-";
    httpLog.error << "HTTP error " << httplib::to_string(err) << " path=" << path;
  });

  // CORS: allow cross-origin requests
  svr.set_default_headers(httplib::Headers{
      {"Access-Control-Allow-Origin", "*"},
      {"Access-Control-Allow-Methods", "GET, POST, OPTIONS"},
      {"Access-Control-Allow-Headers", "Content-Type, Authorization"},
      {"Access-Control-Max-Age", "86400"},
  });
  svr.set_pre_routing_handler([](const httplib::Request& req, httplib::Response& res) {
    if (req.method == "OPTIONS") {
      res.status = 204;
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // Custom HTML for unhandled endpoints (404)
  svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    if (res.status == 404) {
      res.set_content(
        makeErrorHtml(404, req.path, "Page not found",
          "The page you're looking for doesn't exist. This might be a typo, or the endpoint may have moved."),
        "text/html");
      return httplib::Server::HandlerResponse::Handled;
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  // API routes (MCP tools/resources registered alongside corresponding endpoints)
  svr.Get("/api/beacon/state", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconState(req, res, beaconClient);
  });
  registerMcpTool({
    "get_beacon_state",
    "Get the current state of the pp-ledger beacon node (slot, epoch, checkpoint, stakeholders).",
    emptySchema(),
    [](const Object&, pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchBeaconState();
      return r ? mcpOk(metaToJsonString(r.value().ltsToMeta(), 2))
               : mcpErr(r.error().message);
    }
  });
  registerMcpResource({
    "beacon://state",
    "Beacon State",
    "Current state of the pp-ledger beacon node.",
    "application/json",
    [](pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchBeaconState();
      if (!r) {
        Object err;
        err.set("error", r.error().message);
        return err;
      }
      Object content;
      content.set("uri", std::string("beacon://state"));
      content.set("mimeType", std::string("application/json"));
      content.set("text", metaToJsonString(r.value().ltsToMeta(), 2));
      Object out;
      out.set("contents", Object::array({Value(std::make_shared<Object>(content))}));
      return out;
    }
  });

  svr.Get("/api/beacon/calibration", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconCalibration(req, res, beaconClient);
  });

  svr.Get("/api/beacon/miners", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconMiners(req, res, beaconClient);
  });
  registerMcpTool({
    "list_miners",
    "List all miners currently registered with the beacon node.",
    emptySchema(),
    [](const Object&, pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchMinerList();
      if (!r) return mcpErr(r.error().message);
      std::vector<Value> elems;
      for (const auto& m : r.value()) {
        elems.push_back(std::make_shared<Object>(m.ltsToMeta()));
      }
      return mcpOk(dumpJson(Object::array(std::move(elems)), 2));
    }
  });

  svr.Get("/api/miner/status", [&](const httplib::Request& req, httplib::Response& res) {
    handleMinerStatus(req, res, minerClient);
  });
  registerMcpTool({
    "get_miner_status",
    "Get the current status of the connected miner (stake, slot leadership, pending transactions).",
    emptySchema(),
    [](const Object&, pp::Client&, pp::Client& miner) {
      auto r = miner.fetchMinerStatus();
      return r ? mcpOk(metaToJsonString(r.value().ltsToMeta(), 2))
               : mcpErr(r.error().message);
    }
  });
  registerMcpResource({
    "miner://status",
    "Miner Status",
    "Current status of the connected miner.",
    "application/json",
    [](pp::Client&, pp::Client& miner) {
      auto r = miner.fetchMinerStatus();
      if (!r) {
        Object err;
        err.set("error", r.error().message);
        return err;
      }
      Object content;
      content.set("uri", std::string("miner://status"));
      content.set("mimeType", std::string("application/json"));
      content.set("text", metaToJsonString(r.value().ltsToMeta(), 2));
      Object out;
      out.set("contents", Object::array({Value(std::make_shared<Object>(content))}));
      return out;
    }
  });

  svr.Get(R"(/api/block/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    handleBlockGet(req, res, beaconClient);
  });
  {
    Object blockSchema;
    blockSchema.set("type", std::string("object"));
    Object blockIdProp;
    blockIdProp.set("type", std::string("integer"));
    blockIdProp.set("description", std::string("The block ID to fetch"));
    Object props;
    props.set("block_id", blockIdProp);
    blockSchema.set("properties", props);
    blockSchema.set("required", Object::array({Value(std::string("block_id"))}));
    registerMcpTool({
      "get_block",
      "Fetch a block from the pp-ledger blockchain by its block ID.",
      std::move(blockSchema),
      [](const Object& args, pp::Client& beacon, pp::Client&) {
        if (!args.contains("block_id")) return mcpErr("block_id is required");
        auto r = beacon.fetchBlock(objectUint64(args, "block_id", 0));
        return r ? mcpOk(metaToJsonString(r.value().ltsToMeta(), 2))
                 : mcpErr(r.error().message);
      }
    });
  }

  svr.Get(R"(/api/account/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    handleAccountGet(req, res, beaconClient);
  });
  svr.Post("/api/account/create", [&](const httplib::Request& req, httplib::Response& res) {
    handleAccountCreate(req, res, minerClient);
  });
  svr.Get("/api/tx/by-wallet", [&](const httplib::Request& req, httplib::Response& res) {
    handleTxByWallet(req, res, beaconClient);
  });
  svr.Get("/api/tx/by-index", [&](const httplib::Request& req, httplib::Response& res) {
    handleTxByIndex(req, res, beaconClient);
  });
  svr.Post("/api/tx/build", [&](const httplib::Request& req, httplib::Response& res) {
    handleTxBuild(req, res, minerClient);
  });
  svr.Post("/api/tx/submit", [&](const httplib::Request& req, httplib::Response& res) {
    handleTxSubmit(req, res, minerClient);
  });

  // MCP routes
  svr.Get("/mcp/sse", [&](const httplib::Request& req, httplib::Response& res) {
    handleMcpSse(req, res, httpHost, httpPort, mcpSessions, mcpSessionsMutex);
  });
  svr.Post("/mcp/messages", [&](const httplib::Request& req, httplib::Response& res) {
    handleMcpMessages(req, res, beaconClient, minerClient, mcpSessions, mcpSessionsMutex);
  });

  httpLog.info << "HTTP API listening on " << httpHost << ":" << httpPort;
  httpLog.info << "Beacon: " << beaconHost << ":" << beaconPort << "  Miner: " << minerHost << ":" << minerPort;
  httpLog.info << "Routes: GET /api/beacon/state, /api/beacon/calibration, /api/beacon/miners, /api/miner/status, /api/block/<id>, /api/account/<id>";
  httpLog.info << "        POST /api/account/create (JSON: from, amount, key; optional: to, fee, newPubkey, meta, minSignatures)";
  httpLog.info << "        GET /api/tx/by-wallet?walletId=&beforeBlockId=, GET /api/tx/by-index?txIndex=, POST /api/tx/build (JSON), POST /api/tx/submit (JSON)";
  httpLog.info << "MCP:    GET /mcp/sse (SSE endpoint), POST /mcp/messages?sessionId=<id>";
  svr.listen(httpHost, static_cast<int>(httpPort));
  return 0;
}
