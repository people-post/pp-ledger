#include "NetworkTuning.h"

#include <gtest/gtest.h>

using pp::network::NetworkTuning;

// Every deadline derives from the one RPC timeout, so they cannot drift apart.
TEST(NetworkTuningTest, DeadlinesDeriveFromTheRpcTimeout) {
  NetworkTuning t;
  t.rpcTimeout = std::chrono::milliseconds(20000);
  EXPECT_EQ(t.serverQueueExpiry(), std::chrono::milliseconds(10000));
  EXPECT_EQ(t.blockWaitHold(), std::chrono::milliseconds(10000));
  EXPECT_EQ(t.channelReadTimeout(), std::chrono::milliseconds(40000));
  EXPECT_EQ(NetworkTuning::dataTimeoutFor(t.rpcTimeout), std::chrono::milliseconds(40000));
}

TEST(NetworkTuningTest, AmpPolicyReachesTheLinkConfig) {
  NetworkTuning t;
  t.adp.alive_timeout_ms = 9000;
  t.mux.max_concurrent_channels = 32;
  pp::amp::PeerLinkConfig link;
  t.applyTo(link);
  EXPECT_EQ(link.adp.alive_timeout_ms, 9000);
  EXPECT_EQ(link.mux.max_concurrent_channels, 32u);
  EXPECT_TRUE(pp::amp::ValidatePeerLinkConfig(link)); // defaults are a working policy
}
