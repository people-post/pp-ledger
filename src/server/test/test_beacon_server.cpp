#include "BeaconServer.h"
#include "../chain/AccountBuffer.h"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pp;

namespace {

/**
 * A beacon work dir whose init-config.json declares the system accounts by
 * public key (genesis 3 keys, 2-of-3), with each holder's key pair on disk
 * like `pp-client keygen -o` writes them.
 */
struct GenesisSetup {
  std::filesystem::path dir;
  std::vector<utl::MlDsaKeyPair> genesis, fee, reserve, recycle;

  explicit GenesisSetup(const std::string &name) : dir(std::filesystem::temp_directory_path() / name) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 3; ++i) {
      genesis.push_back(utl::mlDsaGenerate().value());
    }
    fee.push_back(utl::mlDsaGenerate().value());
    reserve.push_back(utl::mlDsaGenerate().value());
    recycle.push_back(utl::mlDsaGenerate().value());
  }
  ~GenesisSetup() {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  static std::string pubs(const std::vector<utl::MlDsaKeyPair> &keys) {
    std::string out;
    for (const auto &k : keys) {
      out += (out.empty() ? "\"" : ", \"") + utl::hexEncode(k.publicKey) + "\"";
    }
    return "[" + out + "]";
  }

  void writeInitConfig(const std::string &extra = "") const {
    std::ofstream(dir / "init-config.json")
        << R"({"networkId": "test-net", "systemAccounts": {)"
        << R"("genesis": {"publicKeys": )" << pubs(genesis) << R"(, "minSignatures": 2},)"
        << R"("fee": {"publicKeys": )" << pubs(fee) << R"(},)"
        << R"("reserve": {"publicKeys": )" << pubs(reserve) << R"(},)"
        << R"("recycle": {"publicKeys": )" << pubs(recycle) << R"(}})" << extra << "}";
  }

  /** Write `key`'s private key to a file, as `pp-client keygen -o` does. */
  std::string keyFile(const utl::MlDsaKeyPair &key, const std::string &name) const {
    const auto path = dir / (name + ".key");
    std::ofstream(path) << utl::hexEncode(key.privateKey) << "\n";
    return path.string();
  }

  /** Run --init in its own scope (a live server keeps ledger files open). */
  BeaconServer::Roe<void> init(const std::vector<std::string> &genesisKeyFiles) const {
    BeaconServer server;
    return server.init(dir.string(), genesisKeyFiles);
  }
};

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

// Genesis declares the system accounts by public key; the beacon writes no
// private keys anywhere.
TEST(BeaconServerInitTest, GenesisUsesTheConfiguredPublicKeysAndWritesNoKeys) {
  GenesisSetup setup("pp-ledger-beacon-pubkeys-test");
  setup.writeInitConfig();
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  ASSERT_TRUE(init.isOk()) << init.error().message;

  for (const auto &entry : std::filesystem::directory_iterator(setup.dir)) {
    EXPECT_EQ(entry.path().filename().string().rfind("init-keys", 0), std::string::npos) << entry.path();
  }
  Beacon beacon;
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  ASSERT_TRUE(beacon.mount(mount).isOk());
  EXPECT_EQ(beacon.getNetworkId(), "test-net");
  auto fee = beacon.getAccount(AccountBuffer::ID_FEE);
  ASSERT_TRUE(fee.isOk());
  ASSERT_EQ(fee.value().wallet.publicKeys.size(), 1u);
  EXPECT_EQ(fee.value().wallet.publicKeys[0], setup.fee[0].publicKey);
}

// The genesis key signs genesis M-of-N: too few, foreign or repeated keys fail.
TEST(BeaconServerInitTest, GenesisMustBeSignedByEnoughGenesisKeys) {
  GenesisSetup setup("pp-ledger-beacon-signers-test");
  setup.writeInitConfig();
  const auto g0 = setup.keyFile(setup.genesis[0], "g0");
  const auto g1 = setup.keyFile(setup.genesis[1], "g1");
  const auto foreign = setup.keyFile(setup.fee[0], "fee");

  auto tooFew = setup.init({g0});
  ASSERT_FALSE(tooFew.isOk());
  EXPECT_NE(tooFew.error().message.find("needs 2"), std::string::npos) << tooFew.error().message;

  auto wrong = setup.init({g0, foreign});
  ASSERT_FALSE(wrong.isOk());
  EXPECT_NE(wrong.error().message.find("does not match"), std::string::npos) << wrong.error().message;

  auto repeated = setup.init({g0, g0});
  ASSERT_FALSE(repeated.isOk());
  EXPECT_NE(repeated.error().message.find("twice"), std::string::npos) << repeated.error().message;

  EXPECT_TRUE(setup.init({g0, g1}).isOk());
}

// The genesis (admin) account must be M-of-N from the start (at least 3 keys,
// 2 signatures): the shape its renewals and config updates are held to.
TEST(BeaconServerInitTest, GenesisAccountMustBeMOfN) {
  GenesisSetup setup("pp-ledger-beacon-genesis-shape-test");
  setup.genesis.resize(2); // 2-of-2 parses, but is not the required shape
  setup.writeInitConfig();
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  ASSERT_FALSE(init.isOk());
  EXPECT_NE(init.error().message.find("at least 3"), std::string::npos) << init.error().message;
}

// Without init-config.json, --init writes a template to fill in and stops.
TEST(BeaconServerInitTest, MissingInitConfigWritesATemplateAndStops) {
  GenesisSetup setup("pp-ledger-beacon-template-test");
  auto init = setup.init({});
  ASSERT_FALSE(init.isOk());
  EXPECT_NE(init.error().message.find("keygen"), std::string::npos) << init.error().message;
  EXPECT_TRUE(std::filesystem::exists(setup.dir / "init-config.json"));
}

// Genesis miners must sit in the issued range; anything else is refused at init.
TEST(BeaconServerInitTest, GenesisMinersOutsideTheIssuedRangeAreRefused) {
  for (const uint64_t badId : {uint64_t{5}, AccountBuffer::ID_FIRST_USER}) {
    GenesisSetup setup("pp-ledger-beacon-genesis-miner-test");
    auto key = utl::mlDsaGenerate().value();
    setup.writeInitConfig(R"(, "genesisMiners": [{"id": )" + std::to_string(badId) + R"(, "publicKeys": [")" +
                          utl::hexEncode(key.publicKey) + R"("]}])");
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_FALSE(init.isOk()) << badId;
    EXPECT_NE(init.error().message.find("issued range"), std::string::npos) << init.error().message;
  }
}

// networkId is part of genesis: init refuses to make a chain without one.
TEST(BeaconServerInitTest, InitRequiresNetworkId) {
  GenesisSetup setup("pp-ledger-beacon-network-id-test");
  std::ofstream(setup.dir / "init-config.json") << R"({"slotDuration": 5})";
  auto init = setup.init({});
  ASSERT_FALSE(init.isOk());
  EXPECT_NE(init.error().message.find("networkId"), std::string::npos) << init.error().message;
}

// --- Signed REGISTER: only the miner's own keys can register it ---

TEST(BeaconServerInitTest, RegistrationMustBeSignedByTheMiner) {
  GenesisSetup setup("pp-ledger-beacon-register-test");
  setup.writeInitConfig();
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());

  Beacon beacon;
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  ASSERT_TRUE(beacon.mount(mount).isOk());
  const std::string net = beacon.getNetworkId();

  Client::MinerInfo miner;
  miner.id = AccountBuffer::ID_FEE; // any account with keys works for the signature check
  auto genuine = signedBy(miner, setup.fee, net);
  EXPECT_TRUE(beacon.verifyMinerRegistration(genuine, net).isOk());
  EXPECT_FALSE(beacon.verifyMinerRegistration(genuine, net + "-other").isOk());               // other network
  EXPECT_FALSE(beacon.verifyMinerRegistration(signedBy(miner, setup.reserve, net), net).isOk()); // not its keys

  auto replayedLater = genuine;
  replayedLater.issuedAt += 60; // altered after signing (to dodge the replay check)
  EXPECT_FALSE(beacon.verifyMinerRegistration(replayedLater, net).isOk());
}

// --- Relay / miner without config: a template to fill in, never a guess ---

#include "MinerServer.h"
#include "RelayServer.h"

namespace {

std::string readAll(const std::filesystem::path &path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

template <typename ServerT> std::string runOnce(const std::filesystem::path &dir) {
  ServerT server;
  auto run = server.run(dir.string());
  return run ? std::string{} : run.error().message;
}

} // namespace

TEST(RunConfigTest, RelayWithoutConfigWritesATemplateAndStops) {
  GenesisSetup scratch("pp-ledger-relay-template-test");
  const std::string first = runOnce<RelayServer>(scratch.dir);
  EXPECT_NE(first.find("beacon"), std::string::npos) << first;
  const std::string tmpl = readAll(scratch.dir / "config.json");
  EXPECT_NE(tmpl.find("<upstream multiaddr"), std::string::npos) << tmpl; // a placeholder, not an address
  EXPECT_EQ(tmpl.find("127.0.0.1"), std::string::npos) << tmpl;

  const std::string again = runOnce<RelayServer>(scratch.dir); // template left unedited
  EXPECT_NE(again.find("beacon"), std::string::npos) << again;
}

TEST(RunConfigTest, MinerWithoutConfigWritesATemplateAndStops) {
  GenesisSetup scratch("pp-ledger-miner-template-test");
  const std::string first = runOnce<MinerServer>(scratch.dir);
  EXPECT_NE(first.find("minerId"), std::string::npos) << first;
  const std::string again = runOnce<MinerServer>(scratch.dir); // minerId 0 left as is
  EXPECT_NE(again.find("system accounts"), std::string::npos) << again;
}
