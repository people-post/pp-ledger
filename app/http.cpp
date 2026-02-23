/**
 * HTTP server that exposes the same interfaces as pp::Client.
 * Proxies requests to configured beacon and miner endpoints.
 */
#include "Client.h"
#include "../lib/BinaryPack.hpp"
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
  beaconClient.setEndpoint(pp::network::TcpEndpoint{beaconHost, beaconPort});
  pp::Client minerClient;
  minerClient.setEndpoint(pp::network::TcpEndpoint{minerHost, minerPort});

  httplib::Server svr;

  // GET /beacon/state
  svr.Get("/beacon/state", [&](const httplib::Request&, httplib::Response& res) {
    auto r = beaconClient.fetchBeaconState();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().ltsToJson().dump(), "application/json");
  });

  // GET /beacon/calibration
  svr.Get("/beacon/calibration", [&](const httplib::Request&, httplib::Response& res) {
    auto r = beaconClient.fetchCalibration();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /beacon/miners
  svr.Get("/beacon/miners", [&](const httplib::Request&, httplib::Response& res) {
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

  // GET /miner/status
  svr.Get("/miner/status", [&](const httplib::Request&, httplib::Response& res) {
    auto r = minerClient.fetchMinerStatus();
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().ltsToJson().dump(), "application/json");
  });

  // GET /block/:id
  svr.Get(R"(/block/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t blockId = std::stoull(req.matches[1].str());
    auto r = beaconClient.fetchBlock(blockId);
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /account/:id
  svr.Get(R"(/account/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
    uint64_t accountId = std::stoull(req.matches[1].str());
    auto r = beaconClient.fetchUserAccount(accountId);
    if (!r) {
      setJsonError(res, 502, r.error().message);
      return;
    }
    res.set_content(r.value().toJson().dump(), "application/json");
  });

  // GET /tx/by-wallet?walletId=<id>&beforeBlockId=<id>
  svr.Get("/tx/by-wallet", [&](const httplib::Request& req, httplib::Response& res) {
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

  // POST /tx — body: binary packed SignedData<Transaction> (application/octet-stream)
  svr.Post("/tx", [&](const httplib::Request& req, httplib::Response& res) {
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

  std::cout << "HTTP API listening on " << httpHost << ":" << httpPort << "\n";
  std::cout << "Beacon: " << beaconHost << ":" << beaconPort << "  Miner: " << minerHost << ":" << minerPort << "\n";
  std::cout << "Routes: GET /beacon/state, /beacon/timestamp, /beacon/miners, /miner/status, /block/<id>, /account/<id>\n";
  std::cout << "        GET /tx/by-wallet?walletId=&beforeBlockId=, POST /tx (binary body)\n";
  svr.listen(httpHost, static_cast<int>(httpPort));
  return 0;
}
