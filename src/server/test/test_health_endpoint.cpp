#include "HealthEndpoint.h"

#include "httplib.h"

#include <gtest/gtest.h>

using namespace pp;

namespace {

HealthSnapshot relaySnapshot(bool ready) {
  HealthSnapshot s;
  s.role = "relay";
  s.peerId = "QmRelay";
  s.networkId = "net\"x";
  s.ready = ready;
  s.notReady = ready ? "" : "starting";
  s.nextBlockId = 40;
  s.upstreamNextBlockId = 42;
  s.lastSyncAgeSeconds = 3;
  s.signedTipHeight = 41;
  s.generation = 2;
  s.requestQueueDepth = 5;
  return s;
}

} // namespace

// Probes: /healthz while the server thread reports, /readyz when it says ready; /metrics always.
TEST(HealthEndpointTest, ProbesFollowWhatTheServerThreadPublishes) {
  HealthEndpoint endpoint;
  ASSERT_TRUE(endpoint.start("127.0.0.1", 0).isOk());
  ASSERT_GT(endpoint.port(), 0);
  httplib::Client client("127.0.0.1", endpoint.port());

  auto health = client.Get("/healthz");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 503) << "nothing published yet";

  endpoint.publish(relaySnapshot(false));
  health = client.Get("/healthz");
  ASSERT_TRUE(health);
  EXPECT_EQ(health->status, 200);
  auto ready = client.Get("/readyz");
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready->status, 503);
  EXPECT_NE(ready->body.find("starting"), std::string::npos) << ready->body;

  endpoint.publish(relaySnapshot(true));
  ready = client.Get("/readyz");
  ASSERT_TRUE(ready);
  EXPECT_EQ(ready->status, 200);

  auto metrics = client.Get("/metrics");
  ASSERT_TRUE(metrics);
  EXPECT_EQ(metrics->status, 200);
  const std::string &body = metrics->body;
  EXPECT_NE(body.find("pp_ledger_info{role=\"relay\",peer_id=\"QmRelay\",network=\"net\\\"x\"} 1"),
            std::string::npos)
      << body;
  EXPECT_NE(body.find("pp_ledger_ready 1"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_sync_lag_blocks 2"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_beacon_generation 2"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_request_queue_depth 5"), std::string::npos);
  endpoint.stop();
}

TEST(HealthEndpointTest, AStaleSnapshotIsNeitherUpNorReady) {
  const std::string body = HealthEndpoint::renderMetrics(relaySnapshot(true), /*fresh=*/false);
  EXPECT_NE(body.find("pp_ledger_up 0"), std::string::npos);
  EXPECT_NE(body.find("pp_ledger_ready 0"), std::string::npos);
}
