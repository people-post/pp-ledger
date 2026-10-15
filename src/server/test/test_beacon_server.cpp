#include "BeaconServer.h"
#include "../chain/AccountBuffer.h"
#include "lib/common/Utilities.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
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
  std::vector<utl::MlDsaKeyPair> genesis, reserve, registrar, fee, recycle, recovery;

  explicit GenesisSetup(const std::string &name) : dir(std::filesystem::temp_directory_path() / name) {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    for (int i = 0; i < 3; ++i) {
      genesis.push_back(utl::mlDsaGenerate().value());
    }
    fee.push_back(utl::mlDsaGenerate().value());
    reserve.push_back(utl::mlDsaGenerate().value());
    registrar.push_back(utl::mlDsaGenerate().value());
    recycle.push_back(utl::mlDsaGenerate().value());
    recovery.push_back(utl::mlDsaGenerate().value());
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

  /** `recovery`: the init-config `recovery` object; empty = this setup's recovery keys; "none" = omit it. */
  void writeInitConfig(const std::string &extra = "", const std::string &recoveryJson = "") const {
    const std::string recoveryField =
        recoveryJson == "none" ? std::string()
                               : R"(, "recovery": )" +
                                     (recoveryJson.empty() ? R"({"publicKeys": )" + pubs(recovery) + "}" : recoveryJson);
    std::ofstream(dir / "init-config.json")
        << R"({"networkId": "test-net", "systemAccounts": {)"
        << R"("genesis": {"publicKeys": )" << pubs(genesis) << R"(, "minSignatures": 2},)"
        << R"("fee": {"publicKeys": )" << pubs(fee) << R"(},)"
        << R"("reserve": {"publicKeys": )" << pubs(reserve) << R"(},)"
        << R"("registrar": {"publicKeys": )" << pubs(registrar) << R"(},)"
        << R"("recycle": {"publicKeys": )" << pubs(recycle) << R"(}})" << recoveryField << extra << "}";
  }

  /** Write `key`'s private key to a file, as `pp-client keygen -o` does. */
  std::string keyFile(const utl::MlDsaKeyPair &key, const std::string &name) const {
    const auto path = dir / (name + ".key");
    std::ofstream(path) << utl::hexEncode(key.privateKey) << "\n";
    return path.string();
  }

  /** The genesis meta of an initialized work dir. */
  Chain::GenesisAccountMeta genesisMeta() const {
    Beacon beacon;
    Beacon::MountConfig mount;
    mount.workDir = (dir / "data").string();
    EXPECT_TRUE(beacon.mount(mount).isOk());
    auto block0 = beacon.readBlock(0);
    EXPECT_TRUE(block0.isOk());
    auto tx = utl::binaryUnpack<Ledger::TxGenesis>(block0.value().block.records.at(0).data);
    Chain::GenesisAccountMeta gm;
    EXPECT_TRUE(tx.isOk() && gm.ltsFromString(tx.value().meta));
    return gm;
  }

  /** Write a key pair as `pp-client keygen -o <dir>/<stem>` does. */
  void writeKeyPair(const utl::MlDsaKeyPair &key, const std::string &stem, bool withPub = true) const {
    std::filesystem::create_directories((dir / stem).parent_path());
    std::ofstream(dir / (stem + ".key")) << utl::hexEncode(key.privateKey) << "\n";
    if (withPub) {
      std::ofstream(dir / (stem + ".pub")) << utl::hexEncode(key.publicKey) << "\n";
    }
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
  // The registrar starts empty; reserve funds it later.
  auto registrar = beacon.getAccount(AccountBuffer::ID_REGISTRAR);
  ASSERT_TRUE(registrar.isOk());
  EXPECT_EQ(registrar.value().wallet.publicKeys, std::vector<std::string>{setup.registrar[0].publicKey});
  const auto &balances = registrar.value().wallet.mBalances;
  EXPECT_TRUE(balances.empty() || balances.at(AccountBuffer::ID_GENESIS) == 0);
}

// beacon-authority ba1: --init writes the beacon signing key pair and the genesis record carries the
// recovery set and the generation-1 certificate over that key (delay defaults to ~30 days).
TEST(BeaconServerInitTest, InitWritesTheSigningKeyAndGenesisCarriesTheAuthority) {
  GenesisSetup setup("pp-ledger-beacon-authority-test");
  setup.writeInitConfig();
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  ASSERT_TRUE(init.isOk()) << init.error().message;
  ASSERT_TRUE(std::filesystem::exists(setup.dir / BeaconServer::FILE_BEACON_SIGNING_KEY));
  std::ifstream pubFile(setup.dir / BeaconServer::FILE_BEACON_SIGNING_PUB);
  std::string pubHex;
  pubFile >> pubHex;

  const Chain::GenesisAccountMeta gm = setup.genesisMeta();
  const auto &authority = gm.authority;
  EXPECT_EQ(authority.recovery.publicKeys, std::vector<std::string>{setup.recovery[0].publicKey});
  EXPECT_EQ(authority.recovery.minSignatures, 1u);
  const uint64_t epochSeconds = gm.config.slotDuration * gm.config.slotsPerEpoch;
  EXPECT_EQ(authority.recovery.delayEpochs,
            std::max<uint64_t>(1, (BeaconServer::DEFAULT_RECOVERY_DELAY_SECONDS + epochSeconds - 1) / epochSeconds));
  EXPECT_EQ(authority.beacon.generation, 1u);
  EXPECT_EQ(authority.beacon.cutoverHeight, 0u);
  EXPECT_EQ(authority.beacon.networkId, "test-net");
  EXPECT_EQ(authority.beacon.publicKey, utl::hexDecode(pubHex));
}

// An operator may generate the signing key elsewhere first (pp-client keygen -o keys/beacon-signing).
TEST(BeaconServerInitTest, InitReusesAPreGeneratedSigningKey) {
  GenesisSetup setup("pp-ledger-beacon-signing-reuse-test");
  const auto signing = utl::mlDsaGenerate().value();
  setup.writeKeyPair(signing, "keys/beacon-signing");
  setup.writeInitConfig();
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  ASSERT_TRUE(init.isOk()) << init.error().message;
  EXPECT_EQ(setup.genesisMeta().authority.beacon.publicKey, signing.publicKey);
}

TEST(BeaconServerInitTest, InitRefusesAHalfOrMismatchedSigningKey) {
  {
    GenesisSetup setup("pp-ledger-beacon-signing-half-test");
    setup.writeKeyPair(utl::mlDsaGenerate().value(), "keys/beacon-signing", false);
    setup.writeInitConfig();
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_FALSE(init.isOk());
    EXPECT_NE(init.error().message.find("needs both"), std::string::npos) << init.error().message;
  }
  {
    GenesisSetup setup("pp-ledger-beacon-signing-mismatch-test");
    const auto a = utl::mlDsaGenerate().value();
    const auto b = utl::mlDsaGenerate().value();
    setup.writeKeyPair(a, "keys/beacon-signing", false);
    std::ofstream(setup.dir / BeaconServer::FILE_BEACON_SIGNING_PUB) << utl::hexEncode(b.publicKey) << "\n";
    setup.writeInitConfig();
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_FALSE(init.isOk());
    EXPECT_NE(init.error().message.find("do not match"), std::string::npos) << init.error().message;
  }
}

TEST(BeaconServerInitTest, InitRefusesABadRecoverySet) {
  auto refused = [](const std::string &name, const std::function<std::string(const GenesisSetup &)> &recovery,
                    const std::string &why) {
    GenesisSetup setup(name);
    setup.writeInitConfig("", recovery(setup));
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_FALSE(init.isOk()) << why;
    EXPECT_NE(init.error().message.find(why), std::string::npos) << init.error().message;
  };
  refused("pp-ledger-beacon-recovery-missing-test", [](const GenesisSetup &) { return std::string("none"); },
          "Field 'recovery'");
  refused("pp-ledger-beacon-recovery-admin-test",
          [](const GenesisSetup &s) { return R"({"publicKeys": )" + GenesisSetup::pubs({s.genesis[2]}) + "}"; },
          "must not be an admin");
  refused("pp-ledger-beacon-recovery-delay-test",
          [](const GenesisSetup &s) {
            return R"({"publicKeys": )" + GenesisSetup::pubs(s.recovery) + R"(, "delayEpochs": 0})";
          },
          "delayEpochs must be a positive integer");
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

// Genesis miners are the committee of a new chain: registered as miners from block 0.
TEST(BeaconServerInitTest, GenesisMinersAreRegistered) {
  GenesisSetup setup("pp-ledger-beacon-genesis-miner-registered-test");
  auto key = utl::mlDsaGenerate().value();
  const uint64_t minerId = AccountBuffer::ID_FIRST_ISSUED;
  setup.writeInitConfig(R"(, "genesisMiners": [{"id": )" + std::to_string(minerId) + R"(, "publicKeys": [")" +
                        utl::hexEncode(key.publicKey) + R"("]}])");
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  ASSERT_TRUE(init.isOk()) << init.error().message;
  Beacon beacon;
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  ASSERT_TRUE(beacon.mount(mount).isOk());
  auto miner = beacon.getAccount(minerId);
  ASSERT_TRUE(miner.isOk());
  EXPECT_TRUE(miner.value().wallet.miner.registered);
  EXPECT_FALSE(beacon.getAccount(AccountBuffer::ID_RESERVE).value().wallet.miner.registered);
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

// beacon-authority ba3: once mounted with its signing key, the beacon signs its committed tip (the
// genesis block after --init) and keeps it across restarts; it refuses a ledger that no longer
// holds the block it signed, and signs nothing with a key the chain does not certify.
TEST(BeaconServerInitTest, MountedBeaconSignsItsTipAndKeepsIt) {
  GenesisSetup setup("pp-ledger-beacon-signed-tip-test");
  setup.writeInitConfig();
  const auto beaconKey = utl::mlDsaGenerate().value();
  setup.writeKeyPair(beaconKey, "keys/beacon-signing");
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  const auto authority = setup.genesisMeta().authority;

  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  Client::SignedTip first;
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    EXPECT_FALSE(beacon.getSignedTip().has_value()) << "no signing before openTipSigning";
    ASSERT_TRUE(beacon.openTipSigning(beaconKey).isOk());
    ASSERT_TRUE(beacon.getSignedTip().has_value());
    first = beacon.getSignedTip().value();
    EXPECT_EQ(first.height, 0u);
    EXPECT_EQ(first.generation, 1u);
    EXPECT_EQ(first.networkId, "test-net");
    EXPECT_EQ(first.blockHash, beacon.readBlock(0).value().hash);
    EXPECT_TRUE(first.verify(authority.beacon.publicKey)) << "signed by the generation-1 key";
  }
  {
    Beacon beacon; // restart: the persisted tip, not a fresh signature
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning(beaconKey).isOk());
    EXPECT_EQ(beacon.getSignedTip(), first);
  }
  {
    Beacon beacon; // another key: the beacon runs but signs nothing
    ASSERT_TRUE(beacon.mount(mount).isOk());
    std::filesystem::rename(setup.dir / "data" / "signed-tip.dat", setup.dir / "signed-tip.keep");
    ASSERT_TRUE(beacon.openTipSigning(utl::mlDsaGenerate().value()).isOk());
    EXPECT_FALSE(beacon.getSignedTip().has_value());
    std::filesystem::rename(setup.dir / "signed-tip.keep", setup.dir / "data" / "signed-tip.dat");
  }

  auto writeTip = [&](Client::SignedTip tip) {
    std::ofstream(setup.dir / "data" / "signed-tip.dat", std::ios::binary | std::ios::trunc) << tip.ltsToString();
  };
  auto ahead = first;
  ahead.height = 3; // the ledger lost blocks the beacon signed
  writeTip(ahead);
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    auto refused = beacon.openTipSigning(beaconKey);
    ASSERT_FALSE(refused.isOk());
    EXPECT_NE(refused.error().message.find("below the last signed tip"), std::string::npos) << refused.error().message;
  }
  auto replaced = first;
  replaced.blockHash = "another block";
  writeTip(replaced);
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    EXPECT_FALSE(beacon.openTipSigning(beaconKey).isOk());
  }
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

// Liveness settings (WIRE_SCHEMA § Config updates): the miner due period defaults to one day
// of slots, and init refuses heartbeats rarer than an epoch or a due period under 12 hours.
TEST(BeaconServerInitTest, LivenessSettingsDefaultAndLimits) {
  {
    GenesisSetup setup("pp-ledger-beacon-liveness-default-test");
    setup.writeInitConfig(R"(, "slotDuration": 5, "slotsPerEpoch": 100)");
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_TRUE(init.isOk()) << init.error().message;
    Beacon beacon;
    Beacon::MountConfig mount;
    mount.workDir = (setup.dir / "data").string();
    ASSERT_TRUE(beacon.mount(mount).isOk());
    auto genesis = beacon.readBlock(0);
    ASSERT_TRUE(genesis.isOk());
    auto tx = utl::binaryUnpack<Ledger::TxGenesis>(genesis.value().block.records.at(0).data);
    ASSERT_TRUE(tx.isOk());
    Chain::GenesisAccountMeta gm;
    ASSERT_TRUE(gm.ltsFromString(tx.value().meta));
    EXPECT_EQ(gm.config.minerDueAfterSlots, 86400u / 5) << "one day of slots";
    EXPECT_EQ(gm.config.heartbeatSlots, 100u);
  }
  const std::pair<std::string, std::string> refused[] = {
      {R"(, "slotsPerEpoch": 100, "heartbeatSlots": 101)", "heartbeatSlots"},
      {R"(, "slotDuration": 5, "minerDueAfterSlots": 8639)", "minerDueAfterSlots must be 0 or at least 8640"},
  };
  for (const auto &[extra, why] : refused) {
    GenesisSetup setup("pp-ledger-beacon-liveness-limit-test");
    setup.writeInitConfig(extra);
    auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
    ASSERT_FALSE(init.isOk()) << extra;
    EXPECT_NE(init.error().message.find(why), std::string::npos) << init.error().message;
  }
  GenesisSetup setup("pp-ledger-beacon-liveness-off-test");
  setup.writeInitConfig(R"(, "slotDuration": 5, "minerDueAfterSlots": 0, "heartbeatSlots": 0)");
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
  EXPECT_TRUE(init.isOk()) << init.error().message;
}
