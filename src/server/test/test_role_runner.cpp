#include "ledger/server/RoleRunner.h"
#include "ledger/testing/TestAmpHost.h"
#include "ledger/testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <vector>

using namespace pp;
using namespace std::chrono_literals;

// platform-integration pi2: one ledger role run on its work directory until asked to stop — a
// fresh server after each restart (a rewind, B022) — by the apps and by a host (pp-node).

namespace {

/** A server that asks for `restarts` restarts in all, then runs until stopped. */
class ScriptedServer : public Server {
public:
  struct Script {
    std::atomic<int> made{0};
    std::atomic<int> looped{0};
    int restarts{0};
    std::atomic<bool> stopSeenAtStart{false};
    std::function<void()> onMade;
  };

  explicit ScriptedServer(Script &script) : script_(script), index_(++script.made) {}

protected:
  std::string getSignatureFileName() const override { return ".test"; }
  std::string getLogFileName() const override { return "test.log"; }
  std::string getServerName() const override { return "ScriptedServer"; }
  Service::Roe<void> onStart() override {
    script_.stopSeenAtStart = isStopSet();
    return {};
  }
  void runLoop() override {
    ++script_.looped;
    if (index_ <= script_.restarts) {
      requestRestart(0ms);
    }
    while (!isStopSet()) {
      serveRequestsFor(5ms);
    }
  }

private:
  Script &script_;
  const int index_;
};

class RoleRunnerTest : public ::testing::Test {
protected:
  void SetUp() override { dir_ = pp::test::uniqueTempPath("pp-ledger-role-runner-test"); }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  RoleRunner::Options scripted(ScriptedServer::Script &script) {
    RoleRunner::Options options;
    options.workDir = dir_.string();
    options.logFile = "none";
    options.makeServer = [&script]() {
      auto server = std::make_unique<ScriptedServer>(script);
      if (script.onMade) {
        script.onMade();
      }
      return server;
    };
    return options;
  }
  std::filesystem::path dir_;
};

} // namespace

TEST_F(RoleRunnerTest, RunsAFreshServerAfterEachRestart) {
  ScriptedServer::Script script;
  script.restarts = 2;
  RoleRunner runner(scripted(script));
  auto ran = std::async(std::launch::async, [&runner]() { return runner.run(); });
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (script.looped < 3 && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(script.made.load(), 3) << "two restarts, three servers";
  runner.stop();
  ASSERT_EQ(ran.wait_for(5s), std::future_status::ready);
  EXPECT_TRUE(ran.get().isOk());
  EXPECT_EQ(script.made.load(), 3) << "stopped: no other starts";
}

TEST_F(RoleRunnerTest, AStopBeforeRunStartsNothing) {
  ScriptedServer::Script script;
  RoleRunner runner(scripted(script));
  runner.stop();
  EXPECT_TRUE(runner.run().isOk());
  EXPECT_EQ(script.made.load(), 0);
}

// Service::run clears a server's own stop flag before onStart, so a stop between making a server
// and running it was lost; the runner's flag is the server's stop source and holds.
TEST_F(RoleRunnerTest, AStopBetweenMakingAServerAndRunningItHolds) {
  ScriptedServer::Script script;
  RoleRunner *self = nullptr;
  script.onMade = [&self]() { self->stop(); };
  RoleRunner runner(scripted(script));
  self = &runner;
  auto ran = std::async(std::launch::async, [&runner]() { return runner.run(); });
  ASSERT_EQ(ran.wait_for(5s), std::future_status::ready) << "the server does not run on";
  EXPECT_TRUE(ran.get().isOk());
  EXPECT_TRUE(script.stopSeenAtStart.load());
  EXPECT_EQ(script.looped.load(), 0);
}

TEST_F(RoleRunnerTest, ABeaconRunsStandaloneOnly) {
  RoleRunner::Options options;
  options.role = RoleRunner::Role::Beacon;
  options.workDir = dir_.string();
  options.host = network::HostedAmpRuntime::Host{};
  RoleRunner runner(std::move(options));
  auto ran = runner.run();
  ASSERT_FALSE(ran.isOk());
  EXPECT_NE(ran.error().message.find("standalone only"), std::string::npos) << ran.error().message;
}

TEST(RoleRunnerNamesTest, RolesByName) {
  EXPECT_EQ(RoleRunner::roleNamed("relay"), std::optional<RoleRunner::Role>(RoleRunner::Role::Relay));
  EXPECT_EQ(RoleRunner::roleNamed("miner"), std::optional<RoleRunner::Role>(RoleRunner::Role::Miner));
  EXPECT_EQ(RoleRunner::roleNamed("beacon"), std::optional<RoleRunner::Role>(RoleRunner::Role::Beacon));
  EXPECT_FALSE(RoleRunner::roleNamed("gateway").has_value());
  EXPECT_EQ(RoleRunner::nameOf(RoleRunner::Role::Miner), "miner");
}

// pp-node's use: a relay role on the node's runtime, from its own work directory, stopped from the
// node's thread while its startup still waits for an upstream that never answers. The node is told
// when to advertise the ledger's protocols (bound) and when to withdraw them (before they go).
TEST_F(RoleRunnerTest, ARelayRoleOnAHostTakesItsAddressAndStopsPromptly) {
  auto hub = pp::adp::MemoryDatagramIo::MakeHub();
  pp::test::TestAmpHost host;
  host.start(hub, pp::adp::IpEndpoint::V4(10, 5, 0, 1, 1000));
  std::filesystem::create_directories(dir_);
  const std::string nobody =
      "/ip4/10.5.0.9/udp/9/adp/1.0.0/p2p/" + pp::test::freshAmpIdentity().local_peer_id;
  std::ofstream(dir_ / "config.json") << R"({"port": 18519, "beacon": ")" << nobody << R"("})";

  RoleRunner::Options options;
  options.role = RoleRunner::Role::Relay;
  options.workDir = dir_.string();
  options.logFile = "none";
  options.host = host.forLedger();
  std::mutex mu;
  std::vector<bool> listening;
  bool listenFileAtBind = false;
  options.onListening = [&](const bool bound) {
    std::lock_guard<std::mutex> lock(mu);
    listening.push_back(bound);
    if (bound) {
      listenFileAtBind = std::filesystem::exists(dir_ / "listen.multiaddr");
    }
  };
  RoleRunner runner(std::move(options));
  auto ran = std::async(std::launch::async, [&runner]() { return runner.run(); });

  const auto listenFile = dir_ / "listen.multiaddr";
  const auto until = std::chrono::steady_clock::now() + 10s;
  while (!std::filesystem::exists(listenFile) && std::chrono::steady_clock::now() < until) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(std::filesystem::exists(listenFile)) << "serving";
  std::ifstream in(listenFile);
  std::string listen;
  std::getline(in, listen);
  EXPECT_EQ(listen, host.listen) << "the host's address and identity";
  EXPECT_FALSE(std::filesystem::exists(dir_ / "keys" / "amp-identity.txt")) << "no identity of its own";

  const auto stopAt = std::chrono::steady_clock::now();
  runner.stop();
  ASSERT_EQ(ran.wait_for(10s), std::future_status::ready);
  EXPECT_LT(std::chrono::steady_clock::now() - stopAt, 5s) << "a host's shutdown does not wait out the startup";
  auto result = ran.get();
  EXPECT_TRUE(result.isOk()) << (result.isOk() ? "" : result.error().message);
  std::lock_guard<std::mutex> lock(mu);
  EXPECT_EQ(listening, (std::vector<bool>{true, false})) << "bound once, withdrawn once";
  EXPECT_TRUE(listenFileAtBind) << "told once its listener is up";
}
