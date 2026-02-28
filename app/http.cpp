/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 * Also exposes a Model Context Protocol (MCP) server via SSE transport.
 */
#include "Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include "../http/httplib.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <queue>
#include <string>

#include <sodium.h>

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

  // GET /api/beacon/state
  svr.Get("/api/beacon/state", [&](const httplib::Request&, httplib::Response& res) {
    auto r = beaconClient.fetchBeaconState();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().ltsToJson().dump(), "application/json");
  });

  // GET /api/beacon/calibration
  svr.Get("/api/beacon/calibration", [&](const httplib::Request&, httplib::Response& res) {
    auto r = beaconClient.fetchCalibration();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /api/beacon/miners
  svr.Get("/api/beacon/miners", [&](const httplib::Request&, httplib::Response& res) {
    auto r = beaconClient.fetchMinerList();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    json arr = json::array();
    for (const auto& m : r.value())
      arr.push_back(m.ltsToJson());
    res.set_content(arr.dump(), "application/json");
  });

  // GET /api/miner/status
  svr.Get("/api/miner/status", [&](const httplib::Request&, httplib::Response& res) {
    auto r = minerClient.fetchMinerStatus();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().ltsToJson().dump(), "application/json");
  });

  // GET /api/block/:id
  svr.Get(R"(/api/block/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t blockId = std::stoull(req.matches[1].str());
    auto r = beaconClient.fetchBlock(blockId);
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /api/account/:id
  svr.Get(R"(/api/account/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t accountId = std::stoull(req.matches[1].str());
    auto r = beaconClient.fetchUserAccount(accountId);
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /api/tx/by-wallet?walletId=<id>&beforeBlockId=<id>
  svr.Get("/api/tx/by-wallet", [&](const httplib::Request& req, httplib::Response& res) {
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
  });

  // GET /api/tx/by-index?txIndex=<index>
  svr.Get("/api/tx/by-index", [&](const httplib::Request& req, httplib::Response& res) {
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
  });

  // POST /api/tx — body: binary packed SignedData<Transaction> (application/octet-stream)
  svr.Post("/api/tx", [&](const httplib::Request& req, httplib::Response& res) {
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
  });

  // ── MCP endpoints ────────────────────────────────────────────────────────

  // GET /mcp/sse — establish MCP SSE connection
  svr.Get("/mcp/sse", [&](const httplib::Request& req, httplib::Response& res) {
    std::string sessionId = generateSessionId();
    auto session = std::make_shared<McpSession>();
    {
      std::lock_guard<std::mutex> lk(mcpSessionsMutex);
      mcpSessions[sessionId] = session;
    }

    // Build the endpoint URL using the configured host/port
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
        // Send SSE keepalive comment to keep the connection alive
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
  });

  // POST /mcp/messages?sessionId=<id> — receive JSON-RPC messages
  svr.Post("/mcp/messages", [&](const httplib::Request& req, httplib::Response& res) {
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
  });

  httpLog.info << "HTTP API listening on " << httpHost << ":" << httpPort;
  httpLog.info << "Beacon: " << beaconHost << ":" << beaconPort << "  Miner: " << minerHost << ":" << minerPort;
  httpLog.info << "Routes: GET /api/beacon/state, /api/beacon/calibration, /api/beacon/miners, /api/miner/status, /api/block/<id>, /api/account/<id>";
  httpLog.info << "        GET /api/tx/by-wallet?walletId=&beforeBlockId=, GET /api/tx/by-index?txIndex=, POST /api/tx (binary body)";
  httpLog.info << "MCP:    GET /mcp/sse (SSE endpoint), POST /mcp/messages?sessionId=<id>";
  svr.listen(httpHost, static_cast<int>(httpPort));
  return 0;
}
