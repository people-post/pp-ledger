#include "ledger/client/Client.h"
#include "client/HealthMetrics.h"

#include <gtest/gtest.h>

using namespace pp;

namespace {

Client::NodeHealth relayHealth(bool ready) {
  Client::NodeHealth h;
  h.role = "relay";
  h.peerId = "12D3KooWrelay";
  h.networkId = "net\"1";
  h.ready = ready;
  h.notReady = ready ? "" : "not synced from the upstream";
  h.nextBlockId = 1234567;
  h.upstreamNextBlockId = 1234570;
  h.lastSyncAgeSeconds = 4;
  h.signedTipHeight = 1234569;
  h.generation = 2;
  h.requestQueueDepth = 3;
  h.reportAgeMs = 250;
  return h;
}

TEST(NodeHealthTest, RoundTripsThroughMeta) {
  const auto sent = relayHealth(false);
  Client::NodeHealth got;
  ASSERT_TRUE(got.ltsFromMeta(sent.ltsToMeta()));
  EXPECT_EQ(got.role, "relay");
  EXPECT_EQ(got.notReady, sent.notReady);
  EXPECT_EQ(got.upstreamNextBlockId, sent.upstreamNextBlockId);
  EXPECT_EQ(got.lastSyncAgeSeconds, sent.lastSyncAgeSeconds);
  EXPECT_EQ(got.signedTipHeight, sent.signedTipHeight);
  EXPECT_EQ(got.reportAgeMs, 250);
  EXPECT_TRUE(got.live());
  EXPECT_FALSE(got.readyNow());

  Client::NodeHealth beacon;
  beacon.role = "beacon";
  ASSERT_TRUE(got.ltsFromMeta(beacon.ltsToMeta()));
  EXPECT_FALSE(got.upstreamNextBlockId); // absent stays absent
  EXPECT_FALSE(got.lastSyncAgeSeconds);
}

TEST(NodeHealthTest, AStaleReportIsNeitherLiveNorReady) {
  auto h = relayHealth(true);
  EXPECT_TRUE(h.readyNow());
  h.reportAgeMs = Client::NodeHealth::kStaleAfterMs;
  EXPECT_FALSE(h.live());
  EXPECT_FALSE(h.readyNow());
  h.reportAgeMs = -1;
  EXPECT_FALSE(h.live());
}

TEST(HealthMetricsTest, OneSeriesPerNodeAndANodeThatDidNotAnswerIsDown) {
  const std::string body =
      renderHealthMetrics({{"relay", relayHealth(true)}, {"miner", std::nullopt}});
  EXPECT_NE(body.find("pp_ledger_info{node=\"relay\",role=\"relay\",peer_id=\"12D3KooWrelay\",network=\"net\\\"1\"} 1"),
            std::string::npos);
  EXPECT_NE(body.find("pp_ledger_up{node=\"relay\"} 1\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_up{node=\"miner\"} 0\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_ready{node=\"relay\"} 1\n"), std::string::npos);
  EXPECT_EQ(body.find("pp_ledger_ready{node=\"miner\"}"), std::string::npos); // no answer, no sample
  EXPECT_NE(body.find("pp_ledger_next_block_id{node=\"relay\"} 1234567\n"), std::string::npos); // exact
  EXPECT_NE(body.find("pp_ledger_sync_lag_blocks{node=\"relay\"} 3\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_report_age_seconds{node=\"relay\"} 0.250\n"), std::string::npos);
  // Each family's header once.
  EXPECT_EQ(body.find("# TYPE pp_ledger_up gauge"), body.rfind("# TYPE pp_ledger_up gauge"));
}

// load-resilience lr1: the load counters travel and render as Prometheus counters, per request type.
TEST(NodeHealthTest, LoadCountersRoundTripAndRender) {
  Client::NodeHealth beacon;
  beacon.role = "beacon";
  beacon.requests = {{Client::T_REQ_TX_ADD, 12, 30, 1500}, {Client::T_REQ_STATUS, 3, 0, 4}};
  beacon.requestsBusy = 7;
  beacon.requestsRefused = 2;
  beacon.requestsExpired = 1;
  beacon.loopTurnsSlow = 4;
  beacon.loopTurnMaxMs = 2500;
  beacon.rssBytes = 1 << 20;
  beacon.udpReceiveBufferBytes = 8 << 20;
  beacon.udpReceiveDrops = 0;
  beacon.minerPeers = 3;
  beacon.txPoolRecords = 9;
  beacon.txPoolOldestAgeSeconds = 0;
  beacon.txPoolFull = 5;
  beacon.reportAgeMs = 10;
  Client::NodeHealth got;
  ASSERT_TRUE(got.ltsFromMeta(beacon.ltsToMeta()));
  ASSERT_EQ(got.requests.size(), 2u);
  EXPECT_EQ(got.requests[0].type, Client::T_REQ_TX_ADD);
  EXPECT_EQ(got.requests[0].served, 12u);
  EXPECT_EQ(got.requests[0].serviceMs, 1500u);
  EXPECT_EQ(got.requestsBusy, 7u);
  EXPECT_EQ(got.loopTurnMaxMs, 2500u);
  EXPECT_EQ(got.rssBytes, beacon.rssBytes);
  EXPECT_EQ(got.udpReceiveBufferBytes, beacon.udpReceiveBufferBytes);
  EXPECT_EQ(got.udpReceiveDrops, 0u) << "no drop is a figure, not an absence";
  EXPECT_EQ(got.minerPeers, 3u);
  EXPECT_EQ(got.txPoolOldestAgeSeconds, 0);
  EXPECT_EQ(got.txPoolFull, 5u);
  EXPECT_FALSE(got.blocksSealed) << "a beacon seals nothing: absent stays absent";

  const std::string body = renderHealthMetrics({{"beacon", got}, {"relay", relayHealth(true)}});
  EXPECT_NE(body.find("# TYPE pp_ledger_requests_total counter\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_requests_total{node=\"beacon\",type=\"tx_add\"} 12\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_request_service_seconds_total{node=\"beacon\",type=\"tx_add\"} 1.500\n"),
            std::string::npos);
  EXPECT_NE(body.find("pp_ledger_requests_busy_total{node=\"beacon\"} 7\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_requests_busy_total{node=\"relay\"} 0\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_loop_turn_max_seconds{node=\"beacon\"} 2.500\n"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_tx_pool_full_total{node=\"beacon\"} 5\n"), std::string::npos);
  EXPECT_EQ(body.find("pp_ledger_tx_pool_full_total{node=\"relay\"}"), std::string::npos) << "a relay has no pool";
  EXPECT_EQ(Client::requestTypeName(70000), "type_70000");
}

} // namespace

namespace {

// lr8: a miner's STATUS says which network it is on, under the name a beacon's state uses, so a client
// that signs for the network a node reports (pp-client add-tx, pp-http, pp-client load) gets it from
// any role — signed for an empty id, every node refused the transaction.
TEST(MinerStatusTest, SaysWhichNetworkItIsOn) {
  Client::MinerStatus miner;
  miner.minerId = 1048576;
  miner.networkId = "pp-smoke";
  Client::MinerStatus back;
  ASSERT_TRUE(back.ltsFromMeta(miner.ltsToMeta()));
  EXPECT_EQ(back.networkId, "pp-smoke");
  Client::BeaconState read; // what fetchBeaconState / fetchNetworkId make of it
  ASSERT_TRUE(read.ltsFromMeta(miner.ltsToMeta()));
  EXPECT_EQ(read.networkId, "pp-smoke");
}

} // namespace
