#include "BeaconServer.h"
#include "common/io/Json.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace pp;

namespace {

std::string readFile(const std::filesystem::path &path) {
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::string keysJson(const Beacon::InitKeyConfig &keys) {
  return pp::common::io::metaToJsonString(keys.ltsToMeta(), 2) + "\n";
}

} // namespace

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

// Regression: a second --init used to fail writing init-keys.json after the
// chain was already recreated with new keys, losing them.
TEST(BeaconServerInitTest, SecondInitWritesNewKeyFileAndKeepsFirst) {
  const auto workDir =
      std::filesystem::temp_directory_path() / "pp-ledger-beacon-reinit-test";
  std::error_code ec;
  std::filesystem::remove_all(workDir, ec);

  BeaconServer first;
  auto r1 = first.init(workDir.string());
  ASSERT_TRUE(r1.isOk()) << r1.error().message;
  const std::filesystem::path path1 = first.initKeysPath();
  EXPECT_EQ(path1, workDir / "init-keys.json");
  ASSERT_TRUE(std::filesystem::exists(path1));
  const std::string content1 = readFile(path1);
  EXPECT_EQ(content1, keysJson(r1.value()));

  BeaconServer second;
  auto r2 = second.init(workDir.string());
  ASSERT_TRUE(r2.isOk()) << r2.error().message;
  const std::filesystem::path path2 = second.initKeysPath();
  EXPECT_EQ(path2, workDir / "init-keys-2.json");
  ASSERT_TRUE(std::filesystem::exists(path2));
  EXPECT_EQ(readFile(path2), keysJson(r2.value()));
  EXPECT_NE(readFile(path2), content1);
  EXPECT_EQ(readFile(path1), content1);

  const auto perms = std::filesystem::status(path2).permissions();
  EXPECT_EQ(perms & (std::filesystem::perms::group_all |
                     std::filesystem::perms::others_all),
            std::filesystem::perms::none);

  std::filesystem::remove_all(workDir, ec);
}
