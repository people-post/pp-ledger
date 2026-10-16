#include "Client.h"
#include "HealthMetrics.h"

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

} // namespace
