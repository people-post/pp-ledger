/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 */
#include "Client.h"
#include "../lib/BinaryPack.hpp"
#include "../lib/Logger.h"
#include "../lib/Utilities.h"

#include "../http/httplib.h"

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include <iostream>
#include <string>

using json = nlohmann::json;

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

  httpLog.info << "HTTP API listening on " << httpHost << ":" << httpPort;
  httpLog.info << "Beacon: " << beaconHost << ":" << beaconPort << "  Miner: " << minerHost << ":" << minerPort;
  httpLog.info << "Routes: GET /api/beacon/state, /api/beacon/calibration, /api/beacon/miners, /api/miner/status, /api/block/<id>, /api/account/<id>";
  httpLog.info << "        GET /api/tx/by-wallet?walletId=&beforeBlockId=, POST /api/tx (binary body)";
  svr.listen(httpHost, static_cast<int>(httpPort));
  return 0;
}
