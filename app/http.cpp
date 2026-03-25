/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 * Also exposes a Model Context Protocol (MCP) server via SSE transport.
 */
#include "Client.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "lib/common/Logger.h"
#include "lib/common/Utilities.h"

#include "../http/httplib.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <vector>

#include <sodium.h>

static constexpr uint64_t ID_GENESIS = 0;
static constexpr uint64_t ID_FIRST_USER = 1ULL << 20;
static constexpr size_t MAX_MCP_SESSIONS = 4;
static constexpr size_t MAX_MCP_PENDING_EVENTS_PER_SESSION = 256;
static constexpr size_t HTTP_PAYLOAD_MAX_LENGTH = 2 * 1024 * 1024; // 2 MiB

static uint64_t randomAccountId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist(ID_FIRST_USER, UINT64_MAX);
  return dist(gen);
}

static void setValidationWindow(pp::Ledger::Transaction& tx) {
  const int64_t now = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  tx.idempotentId = static_cast<uint64_t>(now) ^ (randomAccountId() & 0xFFFFULL);
  if (tx.idempotentId == 0) tx.idempotentId = 1;
  tx.validationTsMin = now - 60;
  tx.validationTsMax = now + 3600;
}

using json = nlohmann::json;

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

static json makeRpcResult(const json& id, const json& result) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"result", result}};
}

static json makeRpcError(const json& id, int code, const std::string& message) {
  return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

// ── MCP: tool/resource registries ────────────────────────────────────────────

struct McpTool {
  std::string name;
  std::string description;
  json inputSchema;
  std::function<json(const json&, pp::Client&, pp::Client&)> handler;
};

struct McpResource {
  std::string uri;
  std::string name;
  std::string description;
  std::string mimeType;
  std::function<json(pp::Client&, pp::Client&)> handler;
};

static std::vector<McpTool> g_mcpTools;
static std::vector<McpResource> g_mcpResources;

static void registerMcpTool(McpTool tool) {
  g_mcpTools.push_back(std::move(tool));
}

static void registerMcpResource(McpResource resource) {
  g_mcpResources.push_back(std::move(resource));
}

static json mcpOk(const std::string& text) {
  return json{{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", false}};
}

static json mcpErr(const std::string& text) {
  return json{{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", true}};
}

// ── MCP: JSON-RPC dispatcher ────────────────────────────────────────────────

static std::optional<json> handleMcpRpc(const json& req,
                                         pp::Client& beaconClient, pp::Client& minerClient) {
  if (!req.contains("jsonrpc") || req["jsonrpc"] != "2.0" || !req.contains("method"))
    return makeRpcError(nullptr, -32600, "Invalid Request");

  const std::string method = req["method"].get<std::string>();
  const bool isNotification = !req.contains("id");
  const json id     = isNotification ? json(nullptr) : req["id"];
  const json params = req.value("params", json::object());

  // Notifications have no id and require no response
  if (isNotification) return std::nullopt;

  if (method == "initialize") {
    return makeRpcResult(id, {
      {"protocolVersion", "2024-11-05"},
      {"capabilities", {{"tools", json::object()}, {"resources", json::object()}}},
      {"serverInfo", {{"name", "pp-ledger-mcp"}, {"version", "1.0.0"}}}
    });
  }
  if (method == "ping") {
    return makeRpcResult(id, json::object());
  }
  if (method == "tools/list") {
    json tools = json::array();
    for (const auto& t : g_mcpTools)
      tools.push_back({{"name", t.name}, {"description", t.description}, {"inputSchema", t.inputSchema}});
    return makeRpcResult(id, {{"tools", tools}});
  }
  if (method == "tools/call") {
    const std::string name = params.value("name", "");
    const json args = params.value("arguments", json::object());
    for (const auto& t : g_mcpTools) {
      if (t.name == name)
        return makeRpcResult(id, t.handler(args, beaconClient, minerClient));
    }
    return makeRpcResult(id, mcpErr("Unknown tool: " + name));
  }
  if (method == "resources/list") {
    json resources = json::array();
    for (const auto& r : g_mcpResources)
      resources.push_back({{"uri", r.uri}, {"name", r.name}, {"description", r.description}, {"mimeType", r.mimeType}});
    return makeRpcResult(id, {{"resources", resources}});
  }
  if (method == "resources/read") {
    const std::string uri = params.value("uri", "");
    for (const auto& r : g_mcpResources) {
      if (r.uri == uri) {
        json result = r.handler(beaconClient, minerClient);
        if (result.contains("error"))
          return makeRpcError(id, -32602, result["error"].get<std::string>());
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
  res.set_content(json{{"error", message}}.dump(), "application/json");
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
  res.set_content(r.value().ltsToJson().dump(), "application/json");
}

static void handleBeaconCalibration(const httplib::Request&, httplib::Response& res,
                                   pp::Client& beaconClient) {
  auto r = beaconClient.fetchCalibration();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(r.value().toJson().dump(), "application/json");
}

static void handleBeaconMiners(const httplib::Request&, httplib::Response& res,
                               pp::Client& beaconClient) {
  auto r = beaconClient.fetchMinerList();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  json arr = json::array();
  for (const auto& m : r.value())
    arr.push_back(m.ltsToJson());
  res.set_content(arr.dump(), "application/json");
}

static void handleMinerStatus(const httplib::Request&, httplib::Response& res,
                             pp::Client& minerClient) {
  auto r = minerClient.fetchMinerStatus();
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(r.value().ltsToJson().dump(), "application/json");
}

static void handleBlockGet(const httplib::Request& req, httplib::Response& res,
                          pp::Client& beaconClient) {
  uint64_t blockId = std::stoull(req.matches[1].str());
  auto r = beaconClient.fetchBlock(blockId);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(r.value().toJson().dump(), "application/json");
}

static void handleAccountGet(const httplib::Request& req, httplib::Response& res,
                            pp::Client& beaconClient) {
  uint64_t accountId = std::stoull(req.matches[1].str());
  auto r = beaconClient.fetchUserAccount(accountId);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }
  res.set_content(r.value().toJson().dump(), "application/json");
}

static void handleAccountCreate(const httplib::Request& req, httplib::Response& res,
                                pp::Client& minerClient) {
  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception&) {
    setJsonError(res, 400, "Invalid JSON in request body");
    return;
  }
  if (!body.contains("from") || !body.contains("amount") || !body.contains("key")) {
    setJsonError(res, 400, "from, amount, and key are required");
    return;
  }
  uint64_t fromWalletId = body["from"].get<uint64_t>();
  uint64_t amount = body["amount"].get<uint64_t>();
  uint64_t toWalletId = body.value("to", uint64_t(0));
  if (toWalletId == 0) toWalletId = randomAccountId();
  uint64_t fee = body.value("fee", uint64_t(0));
  std::string newPubkeyHex = body.value("newPubkey", "");
  std::string metaDesc = body.value("meta", "");
  uint8_t minSignatures = static_cast<uint8_t>(body.value("minSignatures", 1));

  std::string pubkeyToUse;
  std::string privateKeyToPrint;
  if (!newPubkeyHex.empty()) {
    std::string pk = newPubkeyHex;
    if (pk.size() >= 2 && (pk[0] == '0' && (pk[1] == 'x' || pk[1] == 'X')))
      pk = pk.substr(2);
    std::string decoded = pp::utl::hexDecode(pk);
    if (decoded.size() != 32) {
      setJsonError(res, 400, "newPubkey must be 32 bytes (64 hex chars)");
      return;
    }
    pubkeyToUse = decoded;
  } else {
    auto pair = pp::utl::ed25519Generate();
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
  userAccount.wallet.keyType = pp::Crypto::TK_ED25519;
  userAccount.wallet.mBalances[ID_GENESIS] = static_cast<int64_t>(amount);
  userAccount.meta = metaDesc;

  std::string keyStr = pp::utl::readKey(body["key"].get<std::string>());
  if (keyStr.size() >= 2 && (keyStr[0] == '0' && (keyStr[1] == 'x' || keyStr[1] == 'X')))
    keyStr = keyStr.substr(2);
  std::string privateKey = pp::utl::hexDecode(keyStr);
  if (privateKey.size() != 32) {
    setJsonError(res, 400, "key must be 32 bytes (64 hex chars)");
    return;
  }

  pp::Ledger::SignedData<pp::Ledger::Transaction> signedTx;
  signedTx.obj.type = pp::Ledger::Transaction::T_NEW_USER;
  signedTx.obj.tokenId = ID_GENESIS;
  signedTx.obj.fromWalletId = fromWalletId;
  signedTx.obj.toWalletId = toWalletId;
  signedTx.obj.amount = amount;
  signedTx.obj.fee = fee;
  signedTx.obj.meta = userAccount.ltsToString();
  setValidationWindow(signedTx.obj);
  std::string message = pp::utl::binaryPack(signedTx.obj);
  auto sigResult = pp::utl::ed25519Sign(privateKey, message);
  if (!sigResult) {
    setJsonError(res, 500, std::string("Sign failed: ") + sigResult.error().message);
    return;
  }
  signedTx.signatures = {*sigResult};

  auto r = minerClient.addTransaction(signedTx);
  if (!r) {
    setJsonError(res, 502, r.error().message);
    return;
  }

  json resp = {{"newAccountId", toWalletId}};
  if (!privateKeyToPrint.empty()) {
    resp["publicKey"] = pp::utl::hexEncode(pubkeyToUse);
    resp["privateKey"] = privateKeyToPrint;
  }
  res.status = 201;
  res.set_content(resp.dump(), "application/json");
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
  res.set_content(r.value().toJson().dump(), "application/json");
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
  res.set_content(r.value().toJson().dump(), "application/json");
}

static uint64_t jsonToUint64(const json& j, const std::string& key, uint64_t defaultVal) {
  if (!j.contains(key)) return defaultVal;
  const auto& v = j[key];
  if (v.is_number_unsigned()) return v.get<uint64_t>();
  if (v.is_number_integer()) {
    int64_t n = v.get<int64_t>();
    if (n < 0) return defaultVal;
    return static_cast<uint64_t>(n);
  }
  if (v.is_string()) {
    std::string s = v.get<std::string>();
    if (s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
      s = s.substr(2);
    try {
      return std::stoull(s, nullptr, 16);
    } catch (...) {
      return defaultVal;
    }
  }
  return defaultVal;
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
  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception&) {
    setJsonError(res, 400, "Invalid JSON in request body");
    return;
  }
  if (!body.contains("fromWalletId") || !body.contains("toWalletId") || !body.contains("amount")) {
    setJsonError(res, 400, "fromWalletId, toWalletId, and amount are required");
    return;
  }

  pp::Ledger::Transaction tx;
  tx.type = static_cast<uint16_t>(jsonToUint64(body, "type", pp::Ledger::Transaction::T_DEFAULT));
  tx.tokenId = jsonToUint64(body, "tokenId", 0);
  tx.fromWalletId = jsonToUint64(body, "fromWalletId", 0);
  tx.toWalletId = jsonToUint64(body, "toWalletId", 0);
  tx.amount = jsonToUint64(body, "amount", 0);
  tx.fee = jsonToUint64(body, "fee", 0);

  if (body.contains("metaHex") && body["metaHex"].is_string()) {
    std::string metaHex = body["metaHex"].get<std::string>();
    if (!metaHex.empty()) {
      if (!isHexStringStrict(metaHex)) {
        setJsonError(res, 400, "metaHex must be an even-length hex string without 0x prefix");
        return;
      }
      tx.meta = pp::utl::hexDecode(metaHex);
    }
  }

  if (body.contains("idempotentId")) {
    tx.idempotentId = jsonToUint64(body, "idempotentId", 0);
  }
  if (body.contains("validationTsMin")) {
    tx.validationTsMin = static_cast<int64_t>(jsonToUint64(body, "validationTsMin", 0));
  }
  if (body.contains("validationTsMax")) {
    tx.validationTsMax = static_cast<int64_t>(jsonToUint64(body, "validationTsMax", 0));
  }
  if (!body.contains("validationTsMin") && !body.contains("validationTsMax")) {
    setValidationWindow(tx);
  }

  std::string unsignedTxPayload = pp::utl::binaryPack(tx);
  json resp = {{"transactionHex", pp::utl::hexEncode(unsignedTxPayload)}};
  res.set_content(resp.dump(), "application/json");
}

static void handleTxSubmit(const httplib::Request& req, httplib::Response& res,
                           pp::Client& minerClient) {
  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception&) {
    setJsonError(res, 400, "Invalid JSON in request body");
    return;
  }
  if (!body.contains("transactionHex") || !body["transactionHex"].is_string()) {
    setJsonError(res, 400, "transactionHex is required and must be a string");
    return;
  }
  if (!body.contains("signaturesHex") || !body["signaturesHex"].is_array()) {
    setJsonError(res, 400, "signaturesHex is required and must be an array");
    return;
  }

  std::string transactionHex = body["transactionHex"].get<std::string>();
  if (!isHexStringStrict(transactionHex)) {
    setJsonError(res, 400, "transactionHex must be a non-empty even-length hex string without 0x prefix");
    return;
  }
  std::string transactionPayload = pp::utl::hexDecode(transactionHex);
  if (transactionPayload.empty()) {
    setJsonError(res, 400, "transactionHex failed to decode");
    return;
  }

  auto txUnpacked = pp::utl::binaryUnpack<pp::Ledger::Transaction>(transactionPayload);
  if (!txUnpacked) {
    setJsonError(res, 400, std::string("Invalid packed transactionHex: ") + txUnpacked.error().message);
    return;
  }

  pp::Ledger::SignedData<pp::Ledger::Transaction> signedTx;
  signedTx.obj = txUnpacked.value();

  const auto& sigsArr = body["signaturesHex"];
  if (sigsArr.empty()) {
    setJsonError(res, 400, "signaturesHex must contain at least one signature");
    return;
  }
  for (const auto& item : sigsArr) {
    if (!item.is_string()) {
      setJsonError(res, 400, "signaturesHex entries must be strings");
      return;
    }
    std::string sigHex = item.get<std::string>();
    if (!isHexStringStrict(sigHex) || sigHex.size() != 128) {
      setJsonError(res, 400, "each signature hex must be exactly 128 hex chars (64 bytes), without 0x prefix");
      return;
    }
    std::string sig = pp::utl::hexDecode(sigHex);
    if (sig.size() != 64) {
      setJsonError(res, 400, "signature hex failed to decode to 64 bytes");
      return;
    }
    signedTx.signatures.push_back(std::move(sig));
  }

  auto r = minerClient.addTransaction(signedTx);
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

  json body;
  try {
    body = json::parse(req.body);
  } catch (const json::exception&) {
    setJsonError(res, 400, "Invalid JSON in request body");
    return;
  }

  bool enqueueFailed = false;
  auto handle = [&](const json& rpc) {
    if (enqueueFailed) return;
    auto response = handleMcpRpc(rpc, beaconClient, minerClient);
    if (response) {
      if (!session->enqueue(makeSseEvent("message", response->dump()))) {
        enqueueFailed = true;
      }
    }
  };

  if (body.is_array()) {
    for (const auto& item : body) handle(item);
  } else {
    handle(body);
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
    {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}},
    [](const json&, pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchBeaconState();
      return r ? mcpOk(r.value().ltsToJson().dump(2)) : mcpErr(r.error().message);
    }
  });
  registerMcpResource({
    "beacon://state",
    "Beacon State",
    "Current state of the pp-ledger beacon node.",
    "application/json",
    [](pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchBeaconState();
      if (!r) return json{{"error", r.error().message}};
      return json{{"contents", json::array({{{"uri", "beacon://state"}, {"mimeType", "application/json"},
                                           {"text", r.value().ltsToJson().dump(2)}}})}};
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
    {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}},
    [](const json&, pp::Client& beacon, pp::Client&) {
      auto r = beacon.fetchMinerList();
      if (!r) return mcpErr(r.error().message);
      json arr = json::array();
      for (const auto& m : r.value()) arr.push_back(m.ltsToJson());
      return mcpOk(arr.dump(2));
    }
  });

  svr.Get("/api/miner/status", [&](const httplib::Request& req, httplib::Response& res) {
    handleMinerStatus(req, res, minerClient);
  });
  registerMcpTool({
    "get_miner_status",
    "Get the current status of the connected miner (stake, slot leadership, pending transactions).",
    {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}},
    [](const json&, pp::Client&, pp::Client& miner) {
      auto r = miner.fetchMinerStatus();
      return r ? mcpOk(r.value().ltsToJson().dump(2)) : mcpErr(r.error().message);
    }
  });
  registerMcpResource({
    "miner://status",
    "Miner Status",
    "Current status of the connected miner.",
    "application/json",
    [](pp::Client&, pp::Client& miner) {
      auto r = miner.fetchMinerStatus();
      if (!r) return json{{"error", r.error().message}};
      return json{{"contents", json::array({{{"uri", "miner://status"}, {"mimeType", "application/json"},
                                            {"text", r.value().ltsToJson().dump(2)}}})}};
    }
  });

  svr.Get(R"(/api/block/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    handleBlockGet(req, res, beaconClient);
  });
  registerMcpTool({
    "get_block",
    "Fetch a block from the pp-ledger blockchain by its block ID.",
    {{"type", "object"},
     {"properties", {{"block_id", {{"type", "integer"}, {"description", "The block ID to fetch"}}}}},
     {"required", json::array({"block_id"})}},
    [](const json& args, pp::Client& beacon, pp::Client&) {
      if (!args.contains("block_id")) return mcpErr("block_id is required");
      auto r = beacon.fetchBlock(args["block_id"].get<uint64_t>());
      return r ? mcpOk(r.value().toJson().dump(2)) : mcpErr(r.error().message);
    }
  });

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
