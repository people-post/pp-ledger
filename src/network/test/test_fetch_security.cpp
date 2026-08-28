#include "FetchServer.h"
#include "FetchServerConfig.h"
#include "TcpClient.h"
#include "platform/ConnectionGuard.h"
#include "platform/NetworkPlatform.h"
#include "SocketTestUtils.h"

#include <gtest/gtest.h>
#include <cstring>
#include <thread>

using namespace pp::network;
using pp::network::testutil::ensureNetworkPlatform;

TEST(FetchSecurityTest, PublicDefaultsCapPayload) {
  const auto cfg = SecurityConfig::publicDefaults();
  EXPECT_EQ(cfg.maxPayloadBytes, 512u * 1024u);
  EXPECT_GT(cfg.readIdleTimeout.count(), 0);
}

TEST(FetchSecurityTest, TrustedDefaultsAllowLargePayload) {
  const auto cfg = SecurityConfig::trustedDefaults();
  EXPECT_EQ(cfg.maxPayloadBytes, LedgerFrameCodec::MAX_PAYLOAD_SIZE);
}

TEST(ConnectionGuardTest, EnforcesPerIpConnectionCap) {
  SecurityConfig cfg = SecurityConfig::publicDefaults();
  cfg.maxConcurrentConnectionsPerIp = 2;
  cfg.maxConnectionsPerIpPerMinute = 1000;
  ConnectionGuard guard(cfg);

  ConnectionRejectReason reason = ConnectionRejectReason::None;
  EXPECT_TRUE(guard.tryAcceptConnection("10.0.0.1", reason));
  EXPECT_TRUE(guard.tryAcceptConnection("10.0.0.1", reason));
  EXPECT_FALSE(guard.tryAcceptConnection("10.0.0.1", reason));
  EXPECT_EQ(reason, ConnectionRejectReason::PerIpCap);

  guard.releaseConnection("10.0.0.1");
  EXPECT_TRUE(guard.tryAcceptConnection("10.0.0.1", reason));
}

TEST(FetchServerSecurityTest, RejectsOversizeLengthPrefix) {
  ensureNetworkPlatform();

  FetchServer server;
  FetchServer::Config config;
  config.endpoint = {"127.0.0.1", 18887};
  config.security = SecurityConfig::publicDefaults();
  config.security.maxPayloadBytes = 64;
  config.handler = [](int, const std::string&, const IpEndpoint&) {};

  ASSERT_TRUE(server.start(config).isOk());
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  TcpClient client;
  ASSERT_TRUE(client.connect(server.getEndpoint()).isOk());

  std::string frame;
  frame.resize(4);
  const uint32_t netLen = htonl(128u);
  std::memcpy(frame.data(), &netLen, sizeof(netLen));
  ASSERT_TRUE(client.send(frame).isOk());

  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  server.stop();
}
