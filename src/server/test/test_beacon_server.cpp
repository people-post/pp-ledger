#include "BeaconServer.h"
#include "common/io/Json.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace pp;

// `whitelist` keeps its IP meaning; AMP PeerId filtering has its own field.
TEST(BeaconServerConfigTest, RunFileConfig_ParsesAmpPeerWhitelistSeparately) {
  pp::common::Object jd;
  ASSERT_TRUE(pp::common::io::objectFromJsonString(
      jd, R"({"host":"127.0.0.1","port":8517,"whitelist":["10.0.0.1"],)"
          R"("ampPeerWhitelist":["peer-a","peer-b"]})"));
  BeaconServer::RunFileConfig cfg;
  auto parsed = cfg.ltsFromJson(jd);
  ASSERT_TRUE(parsed.isOk()) << parsed.error().message;
  EXPECT_EQ(cfg.whitelist, std::vector<std::string>{"10.0.0.1"});
  EXPECT_EQ(cfg.ampPeerWhitelist, (std::vector<std::string>{"peer-a", "peer-b"}));

  BeaconServer::RunFileConfig roundTrip;
  ASSERT_TRUE(roundTrip.ltsFromJson(cfg.ltsToJson()).isOk());
  EXPECT_EQ(roundTrip.whitelist, cfg.whitelist);
  EXPECT_EQ(roundTrip.ampPeerWhitelist, cfg.ampPeerWhitelist);
}

TEST(BeaconServerConfigTest, RunFileConfig_AmpPeerWhitelistDefaultsEmpty) {
  pp::common::Object jd;
  ASSERT_TRUE(pp::common::io::objectFromJsonString(
      jd, R"({"host":"127.0.0.1","port":8517,"whitelist":["10.0.0.1"]})"));
  BeaconServer::RunFileConfig cfg;
  ASSERT_TRUE(cfg.ltsFromJson(jd).isOk());
  EXPECT_TRUE(cfg.ampPeerWhitelist.empty());
  EXPECT_EQ(cfg.whitelist.size(), 1u);
}

TEST(BeaconServerConfigTest, RunFileConfig_RejectsNonStringAmpPeer) {
  pp::common::Object jd;
  ASSERT_TRUE(pp::common::io::objectFromJsonString(
      jd, R"({"host":"127.0.0.1","port":8517,"ampPeerWhitelist":[1]})"));
  BeaconServer::RunFileConfig cfg;
  EXPECT_FALSE(cfg.ltsFromJson(jd).isOk());
}
