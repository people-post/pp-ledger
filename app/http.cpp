/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 * Also exposes a Model Context Protocol (MCP) server via SSE transport.
 */
#include "Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Crypto.h"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include "../http/httplib.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <random>
#include <string>

#include <sodium.h>

static constexpr uint64_t ID_GENESIS = 0;
static constexpr uint64_t ID_FIRST_USER = 1ULL << 20;

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
  bool                    closed{false};

  void enqueue(std::string event) {
    std::lock_guard<std::mutex> lk(mutex);
    pending.push(std::move(event));
    cv.notify_one();
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

// ── MCP: tools & resources ──────────────────────────────────────────────────

static json buildToolsList() {
  return json::array({
    {{"name", "get_beacon_state"},
     {"description", "Get the current state of the pp-ledger beacon node (slot, epoch, checkpoint, stakeholders)."},
     {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}},
    {{"name", "get_miner_status"},
     {"description", "Get the current status of the connected miner (stake, slot leadership, pending transactions)."},
     {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}},
    {{"name", "list_miners"},
     {"description", "List all miners currently registered with the beacon node."},
     {"inputSchema", {{"type", "object"}, {"properties", json::object()}, {"required", json::array()}}}},
    {{"name", "get_block"},
     {"description", "Fetch a block from the pp-ledger blockchain by its block ID."},
     {"inputSchema", {{"type", "object"},
       {"properties", {{"block_id", {{"type", "integer"}, {"description", "The block ID to fetch"}}}}},
       {"required", json::array({"block_id"})}}}}
  });
}

static json buildResourcesList() {
  return json::array({
    {{"uri", "beacon://state"},
     {"name", "Beacon State"},
     {"description", "Current state of the pp-ledger beacon node."},
     {"mimeType", "application/json"}},
    {{"uri", "miner://status"},
     {"name", "Miner Status"},
     {"description", "Current status of the connected miner."},
     {"mimeType", "application/json"}}
  });
}

static json callMcpTool(const std::string& name, const json& args,
                         pp::Client& beaconClient, pp::Client& minerClient) {
  auto ok  = [](const std::string& text) {
    return json{{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", false}};
  };
  auto err = [](const std::string& text) {
    return json{{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", true}};
  };

  if (name == "get_beacon_state") {
    auto r = beaconClient.fetchBeaconState();
    return r ? ok(r.value().ltsToJson().dump(2)) : err(r.error().message);
  }
  if (name == "get_miner_status") {
    auto r = minerClient.fetchMinerStatus();
    return r ? ok(r.value().ltsToJson().dump(2)) : err(r.error().message);
  }
  if (name == "list_miners") {
    auto r = beaconClient.fetchMinerList();
    if (!r) return err(r.error().message);
    json arr = json::array();
    for (const auto& m : r.value()) arr.push_back(m.ltsToJson());
    return ok(arr.dump(2));
  }
  if (name == "get_block") {
    if (!args.contains("block_id")) return err("block_id is required");
    auto r = beaconClient.fetchBlock(args["block_id"].get<uint64_t>());
    return r ? ok(r.value().toJson().dump(2)) : err(r.error().message);
  }
  return err("Unknown tool: " + name);
}

static json readMcpResource(const std::string& uri,
                             pp::Client& beaconClient, pp::Client& minerClient) {
  if (uri == "beacon://state") {
    auto r = beaconClient.fetchBeaconState();
    if (!r) return {{"error", r.error().message}};
    return {{"contents", json::array({{{"uri", uri}, {"mimeType", "application/json"},
                                       {"text", r.value().ltsToJson().dump(2)}}})}};
  }
  if (uri == "miner://status") {
    auto r = minerClient.fetchMinerStatus();
    if (!r) return {{"error", r.error().message}};
    return {{"contents", json::array({{{"uri", uri}, {"mimeType", "application/json"},
                                       {"text", r.value().ltsToJson().dump(2)}}})}};
  }
  return {{"error", "Unknown resource: " + uri}};
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
    return makeRpcResult(id, {{"tools", buildToolsList()}});
  }
  if (method == "tools/call") {
    const std::string name = params.value("name", "");
    const json args = params.value("arguments", json::object());
    return makeRpcResult(id, callMcpTool(name, args, beaconClient, minerClient));
  }
  if (method == "resources/list") {
    return makeRpcResult(id, {{"resources", buildResourcesList()}});
  }
  if (method == "resources/read") {
    const std::string uri = params.value("uri", "");
    json result = readMcpResource(uri, beaconClient, minerClient);
    if (result.contains("error"))
      return makeRpcError(id, -32602, result["error"].get<std::string>());
    return makeRpcResult(id, result);
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

static void handleTxPost(const httplib::Request& req, httplib::Response& res,
                        pp::Client& minerClient) {
  const std::string& body = req.body;
  auto unpacked = pp::utl::binaryUnpack<pp::Ledger::SignedData<pp::Ledger::Transaction>>(body);
  if (!unpacked) {
    setJsonError(res, 400, std::string("Invalid signed tx: ") + unpacked.error().message);
    return;
  }
  auto r = minerClient.addTransaction(unpacked.value());
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
  std::string sessionId = generateSessionId();
  auto session = std::make_shared<McpSession>();
  {
    std::lock_guard<std::mutex> lk(mcpSessionsMutex);
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

  auto handle = [&](const json& rpc) {
    auto response = handleMcpRpc(rpc, beaconClient, minerClient);
    if (response) session->enqueue(makeSseEvent("message", response->dump()));
  };

  if (body.is_array()) {
    for (const auto& item : body) handle(item);
  } else {
    handle(body);
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

  // API routes
  svr.Get("/api/beacon/state", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconState(req, res, beaconClient);
  });
  svr.Get("/api/beacon/calibration", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconCalibration(req, res, beaconClient);
  });
  svr.Get("/api/beacon/miners", [&](const httplib::Request& req, httplib::Response& res) {
    handleBeaconMiners(req, res, beaconClient);
  });
  svr.Get("/api/miner/status", [&](const httplib::Request& req, httplib::Response& res) {
    handleMinerStatus(req, res, minerClient);
  });
  svr.Get(R"(/api/block/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    handleBlockGet(req, res, beaconClient);
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
  svr.Post("/api/tx", [&](const httplib::Request& req, httplib::Response& res) {
    handleTxPost(req, res, minerClient);
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
  httpLog.info << "        GET /api/tx/by-wallet?walletId=&beforeBlockId=, GET /api/tx/by-index?txIndex=, POST /api/tx (binary body)";
  httpLog.info << "MCP:    GET /mcp/sse (SSE endpoint), POST /mcp/messages?sessionId=<id>";
  svr.listen(httpHost, static_cast<int>(httpPort));
  return 0;
}
