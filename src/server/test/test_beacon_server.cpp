#include "BeaconServer.h"
#include "../chain/AccountBuffer.h"
#include "../network/amp/LedgerPeerId.h"
#include "amp/link/AdpMultiaddr.h"
#include "lib/common/Utilities.h"
#include "common/io/Json.h"

#include <gtest/gtest.h>

#include <chrono>
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

// Regression: a second --init used to fail writing init-keys.json after the
// chain was already recreated with new keys, losing them.
TEST(BeaconServerInitTest, SecondInitWritesNewKeyFileAndKeepsFirst) {
  const auto workDir =
      std::filesystem::temp_directory_path() / "pp-ledger-beacon-reinit-test";
  std::error_code ec;
  std::filesystem::remove_all(workDir, ec);

  // Each init runs in its own scope, like separate `pp-beacon --init` runs:
  // a live server keeps ledger files open, which blocks the next init's
  // cleanup on Windows.
  std::filesystem::path path1;
  std::string content1;
  {
    BeaconServer first;
    auto r1 = first.init(workDir.string());
    ASSERT_TRUE(r1.isOk()) << r1.error().message;
    path1 = first.initKeysPath();
    content1 = keysJson(r1.value());
  }
  EXPECT_EQ(path1, workDir / "init-keys.json");
  ASSERT_TRUE(std::filesystem::exists(path1));
  EXPECT_EQ(readFile(path1), content1);

  std::filesystem::path path2;
  std::string content2;
  {
    BeaconServer second;
    auto r2 = second.init(workDir.string());
    ASSERT_TRUE(r2.isOk()) << r2.error().message;
    path2 = second.initKeysPath();
    content2 = keysJson(r2.value());
  }
  EXPECT_EQ(path2, workDir / "init-keys-2.json");
  ASSERT_TRUE(std::filesystem::exists(path2));
  EXPECT_EQ(readFile(path2), content2);
  EXPECT_NE(content2, content1);
  EXPECT_EQ(readFile(path1), content1);

#if !defined(_WIN32)
  // 0600 is POSIX-only (see writeToNewFile).
  const auto perms = std::filesystem::status(path2).permissions();
  EXPECT_EQ(perms & (std::filesystem::perms::group_all |
                     std::filesystem::perms::others_all),
            std::filesystem::perms::none);
#endif

  std::filesystem::remove_all(workDir, ec);
}

// Genesis miners must sit in the issued range; anything else is refused at init.
TEST(BeaconServerInitTest, GenesisMinersOutsideTheIssuedRangeAreRefused) {
  const auto workDir = std::filesystem::temp_directory_path() / "pp-ledger-beacon-genesis-miner-test";
  std::error_code ec;
  for (const uint64_t badId : {uint64_t{5}, AccountBuffer::ID_FIRST_USER}) {
    std::filesystem::remove_all(workDir, ec);
    std::filesystem::create_directories(workDir);
    auto key = utl::mlDsaGenerate().value();
    std::ofstream(workDir / "init-config.json")
        << R"({"genesisMiners": [{"id": )" << badId << R"(, "publicKeys": [")" << utl::hexEncode(key.publicKey)
        << R"("]}]})";
    {
      BeaconServer server;
      auto init = server.init(workDir.string());
      ASSERT_FALSE(init.isOk()) << badId;
      EXPECT_NE(init.error().message.find("issued range"), std::string::npos) << init.error().message;
    }
  }
  std::filesystem::remove_all(workDir, ec);
}

// --- Signed REGISTER: only the miner's own keys can register its address ---

namespace {

std::string endpointFor(const std::string &publicKey) {
  auto peerId = network::PeerIdFromMlDsaPublicKey(std::vector<uint8_t>(publicKey.begin(), publicKey.end()));
  EXPECT_TRUE(peerId.isOk());
  return pp::amp::FormatAdpMultiaddr(pp::adp::IpEndpoint::V4(127, 0, 0, 1, 9000), peerId.value()).value();
}

Client::MinerInfo signedBy(Client::MinerInfo miner, const std::vector<utl::MlDsaKeyPair> &keys,
                           const std::string &networkId) {
  miner.issuedAt = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  miner.signatures.clear();
  for (const auto &key : keys) {
    miner.signatures.push_back(utl::mlDsaSign(key.privateKey, miner.signingMessage(networkId)).value());
  }
  return miner;
}

} // namespace

TEST(BeaconServerInitTest, RegistrationMustBeSignedByTheMinerAndNameItsOwnPeerId) {
  const auto workDir = std::filesystem::temp_directory_path() / "pp-ledger-beacon-register-test";
  std::error_code ec;
  std::filesystem::remove_all(workDir, ec);
  Beacon::InitKeyConfig keys;
  {
    BeaconServer server;
    auto init = server.init(workDir.string());
    ASSERT_TRUE(init.isOk()) << init.error().message;
    keys = init.value();
  }
  {
    Beacon beacon;
    Beacon::MountConfig mount;
    mount.workDir = (workDir / "data").string();
    ASSERT_TRUE(beacon.mount(mount).isOk());
    const std::string net = beacon.getNetworkId();
    ASSERT_FALSE(keys.genesis.empty());
    ASSERT_FALSE(keys.fee.empty());

    Client::MinerInfo miner;
    miner.id = AccountBuffer::ID_GENESIS;
    miner.endpoint = endpointFor(keys.genesis.front().publicKey);

    auto genuine = signedBy(miner, keys.genesis, net);
    EXPECT_TRUE(beacon.verifyMinerRegistration(genuine, net).isOk());
    EXPECT_FALSE(beacon.verifyMinerRegistration(genuine, net + "-other").isOk()); // other network

    EXPECT_FALSE(beacon.verifyMinerRegistration(signedBy(miner, keys.fee, net), net).isOk()); // not its keys

    auto tampered = genuine;
    tampered.endpoint = endpointFor(keys.fee.front().publicKey); // swap the address after signing
    EXPECT_FALSE(beacon.verifyMinerRegistration(tampered, net).isOk());

    Client::MinerInfo otherPeer = miner; // signed by the miner, but naming someone else's peer id
    otherPeer.endpoint = endpointFor(keys.fee.front().publicKey);
    EXPECT_FALSE(beacon.verifyMinerRegistration(signedBy(otherPeer, keys.genesis, net), net).isOk());
  }
  std::filesystem::remove_all(workDir, ec);
}
