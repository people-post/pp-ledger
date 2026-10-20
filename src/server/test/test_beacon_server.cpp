#include "server/BeaconServer.h"
#include "server/CertificateStore.h"
#include "server/EquivocationStore.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/operation/Operation.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"
#include "testing/TestTempPath.h"

#include <gtest/gtest.h>

#include <limits>

#include <algorithm>
#include <chrono>
#include <cstdlib>
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

  explicit GenesisSetup(const std::string &name) : dir(pp::test::uniqueTempPath(name)) {
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

  /** --plan-genesis, then each of `signers` signs its own copy and the copies merge (offline genesis). */
  std::string signedGenesis(int64_t startTime, const std::vector<utl::MlDsaKeyPair> &signers) const {
    const std::string plan = (dir / "plan.json").string();
    std::filesystem::remove(plan);
    BeaconServer planner;
    EXPECT_TRUE(planner.planGenesis(dir.string(), plan, startTime).isOk());
    std::vector<operation::Operation> copies;
    for (const auto &signer : signers) {
      auto copy = operation::load(plan);
      EXPECT_TRUE(copy.isOk());
      EXPECT_TRUE(operation::sign(copy.value(), signer.privateKey).isOk());
      copies.push_back(copy.value());
    }
    const std::string out = (dir / "signed.json").string();
    std::filesystem::remove(out);
    EXPECT_TRUE(operation::save(operation::merge(copies).value(), out).isOk());
    return out;
  }

  BeaconServer::Roe<void> initSigned(const std::string &operationPath) const {
    BeaconServer server;
    return server.initSigned(dir.string(), operationPath);
  }

  /** Run --init in its own scope (a live server keeps ledger files open). */
  BeaconServer::Roe<void> init(const std::vector<std::string> &genesisKeyFiles) const {
    BeaconServer server;
    return server.init(dir.string(), genesisKeyFiles);
  }
};

std::string readAllText(const std::filesystem::path &path) {
  std::ifstream in(path);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
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

// AD003 (offline genesis): the beacon plans block 0, the admins sign it apart, the beacon writes
// it — no genesis private key on this host. The network starts at the planned time.
TEST(BeaconServerInitTest, AGenesisSignedOfflineStartsTheChainAtItsPlannedTime) {
  GenesisSetup setup("pp-ledger-beacon-offline-genesis-test");
  setup.writeInitConfig();
  const int64_t start = utl::getCurrentTime() + 600;
  const std::string signedOp = setup.signedGenesis(start, {setup.genesis[0], setup.genesis[2]});
  auto init = setup.initSigned(signedOp);
  ASSERT_TRUE(init.isOk()) << init.error().message;

  const Chain::GenesisAccountMeta gm = setup.genesisMeta();
  EXPECT_EQ(gm.config.genesisTime, start);
  EXPECT_EQ(gm.genesis.wallet.publicKeys.size(), 3u);
  Beacon beacon;
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  ASSERT_TRUE(beacon.mount(mount).isOk());
  EXPECT_EQ(beacon.getNetworkId(), "test-net");
  auto reserve = beacon.getAccount(AccountBuffer::ID_RESERVE);
  ASSERT_TRUE(reserve.isOk());
  EXPECT_EQ(reserve.value().wallet.publicKeys, std::vector<std::string>{setup.reserve[0].publicKey});
  EXPECT_GT(reserve.value().wallet.mBalances.at(AccountBuffer::ID_GENESIS), 0);
}

// A signed genesis is written only when complete, and only by the beacon whose key it certifies.
TEST(BeaconServerInitTest, AnOfflineGenesisNeedsItsQuorumAndThisBeaconsKey) {
  GenesisSetup setup("pp-ledger-beacon-offline-genesis-refused-test");
  setup.writeInitConfig();
  const std::string oneSignature = setup.signedGenesis(utl::getCurrentTime(), {setup.genesis[1]});
  auto shortOfQuorum = setup.initSigned(oneSignature);
  ASSERT_FALSE(shortOfQuorum.isOk());
  EXPECT_NE(shortOfQuorum.error().message.find("Signed by 1 of the 2"), std::string::npos)
      << shortOfQuorum.error().message;

  const std::string complete = setup.signedGenesis(utl::getCurrentTime(), {setup.genesis[0], setup.genesis[1]});
  std::filesystem::remove(setup.dir / BeaconServer::FILE_BEACON_SIGNING_KEY);
  std::filesystem::remove(setup.dir / BeaconServer::FILE_BEACON_SIGNING_PUB);
  auto otherBeacon = setup.initSigned(complete);
  ASSERT_FALSE(otherBeacon.isOk());
  EXPECT_NE(otherBeacon.error().message.find("plan the genesis with this beacon's key"), std::string::npos)
      << otherBeacon.error().message;
  EXPECT_FALSE(std::filesystem::exists(setup.dir / "data" / "ledger"));
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

// The signing key may be held outside the work directory (PP_BEACON_SIGNING_KEY or _FILE): genesis
// certifies its public key (derived from it) and no key file is written.
TEST(BeaconServerInitTest, InitTakesTheSigningKeyFromTheEnvironment) {
  GenesisSetup setup("pp-ledger-beacon-signing-env-test");
  setup.writeInitConfig();
  const auto signing = utl::mlDsaGenerate().value();
#ifdef _WIN32
  _putenv_s("PP_BEACON_SIGNING_KEY", utl::hexEncode(signing.privateKey).c_str());
#else
  setenv("PP_BEACON_SIGNING_KEY", utl::hexEncode(signing.privateKey).c_str(), 1);
#endif
  auto init = setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")});
#ifdef _WIN32
  _putenv_s("PP_BEACON_SIGNING_KEY", "");
#else
  unsetenv("PP_BEACON_SIGNING_KEY");
#endif
  ASSERT_TRUE(init.isOk()) << init.error().message;
  EXPECT_EQ(setup.genesisMeta().authority.beacon.publicKey, signing.publicKey);
  EXPECT_FALSE(std::filesystem::exists(setup.dir / BeaconServer::FILE_BEACON_SIGNING_KEY));
  EXPECT_FALSE(std::filesystem::exists(setup.dir / BeaconServer::FILE_BEACON_SIGNING_PUB));
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

// --init writes genesis once: a second one is refused (it used to delete the chain) unless --force;
// a failed one leaves no partial chain behind.
TEST(BeaconServerInitTest, InitDoesNotReplaceAChainUnlessForced) {
  GenesisSetup setup("pp-ledger-beacon-reinit-test");
  setup.writeInitConfig();
  const auto g0 = setup.keyFile(setup.genesis[0], "g0");
  const auto g1 = setup.keyFile(setup.genesis[1], "g1");
  ASSERT_FALSE(setup.init({g0}).isOk()) << "too few signatures";
  EXPECT_FALSE(std::filesystem::exists(setup.dir / "data")) << "a failed init leaves nothing";
  ASSERT_TRUE(setup.init({g0, g1}).isOk());
  const auto ledger = setup.dir / "data" / "ledger";
  ASSERT_TRUE(std::filesystem::exists(ledger));

  auto again = setup.init({g0, g1});
  ASSERT_FALSE(again.isOk());
  EXPECT_NE(again.error().message.find("--force"), std::string::npos) << again.error().message;
  EXPECT_TRUE(std::filesystem::exists(ledger)) << "the chain is kept";

  BeaconServer server;
  EXPECT_TRUE(server.init(setup.dir.string(), {g0, g1}, /*force=*/true).isOk());
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
    ASSERT_TRUE(beacon.openTipSigning({beaconKey}).isOk());
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
    ASSERT_TRUE(beacon.openTipSigning({beaconKey}).isOk());
    EXPECT_EQ(beacon.getSignedTip(), first);
  }
  {
    Beacon beacon; // another key: the beacon runs but signs nothing
    ASSERT_TRUE(beacon.mount(mount).isOk());
    std::filesystem::rename(setup.dir / "data" / "signed-tip.dat", setup.dir / "signed-tip.keep");
    ASSERT_TRUE(beacon.openTipSigning({utl::mlDsaGenerate().value()}).isOk());
    EXPECT_FALSE(beacon.getSignedTip().has_value());
    EXPECT_TRUE(beacon.isSuperseded()) << "no key it holds is certified: it stands down";
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
    auto refused = beacon.openTipSigning({beaconKey});
    ASSERT_FALSE(refused.isOk());
    EXPECT_NE(refused.error().message.find("below the last signed tip"), std::string::npos) << refused.error().message;
  }
  auto replaced = first;
  replaced.blockHash = "another block";
  writeTip(replaced);
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    EXPECT_FALSE(beacon.openTipSigning({beaconKey}).isOk());
  }
}

// beacon-authority ba5c: a certificate seen out of band (a TX_ADD) is kept across restarts. Naming a
// key the beacon does not hold, it stands the beacon down at once; naming one it holds (a staged
// rotation), the beacon commits nothing above its cutover but a block that commits a certificate.
TEST(BeaconServerInitTest, ACertificateSeenOutOfBandIsKeptAndBindsTheBeacon) {
  GenesisSetup setup("pp-ledger-beacon-seen-cert-test");
  setup.writeInitConfig();
  const auto beaconKey = utl::mlDsaGenerate().value();
  const auto nextKey = utl::mlDsaGenerate().value();
  setup.writeKeyPair(beaconKey, "keys/beacon-signing");
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  const std::string genesisHash = [&]() {
    Beacon reader;
    Beacon::MountConfig mounted;
    mounted.workDir = (setup.dir / "data").string();
    EXPECT_TRUE(reader.mount(mounted).isOk());
    return reader.readBlock(0).value().hash;
  }();
  auto certFor = [&](const utl::MlDsaKeyPair &key, uint64_t generation) {
    BeaconCertificate cert;
    cert.networkId = "test-net";
    cert.generation = generation;
    cert.publicKey = key.publicKey;
    cert.cutoverHeight = 0; // the old generation signs nothing past genesis
    cert.cutoverHash = genesisHash;
    Ledger::TxBeaconCert tx;
    tx.meta = cert.ltsToString();
    Ledger::Record record;
    record.type = Ledger::T_BEACON_CERT;
    record.data = utl::binaryPack(tx);
    for (int i = 0; i < 2; ++i) {
      record.signatures.push_back(utl::mlDsaSign(setup.genesis[i].privateKey, record.signingMessage("test-net")).value());
    }
    return record;
  };
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  const auto certFile = setup.dir / "data" / "beacon-cert.dat";

  const auto rotation = certFor(nextKey, 2);
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({beaconKey, nextKey}).isOk());
    ASSERT_EQ(beacon.offerCertificate(rotation).value(), Chain::CertificateOffer::Newer);
    EXPECT_FALSE(beacon.isSuperseded()) << "it holds the key generation 2 names";
    EXPECT_TRUE(std::filesystem::exists(certFile));
    ASSERT_TRUE(beacon.getCurrentCertificate().has_value());
    EXPECT_EQ(beacon.getCurrentCertificate()->data, rotation.data);
    Ledger::ChainNode past;
    past.block.index = 1;
    auto refused = beacon.addBlock(past);
    ASSERT_FALSE(refused.isOk());
    EXPECT_NE(refused.error().message.find("must commit a beacon certificate"), std::string::npos)
        << refused.error().message;
  }
  const auto stolen = certFor(utl::mlDsaGenerate().value(), 3);
  {
    Beacon beacon; // restart: generation 2 is still held
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({beaconKey, nextKey}).isOk());
    ASSERT_TRUE(beacon.getCurrentCertificate().has_value()) << "kept across the restart";
    EXPECT_EQ(beacon.getCurrentCertificate()->data, rotation.data);
    ASSERT_EQ(beacon.offerCertificate(stolen).value(), Chain::CertificateOffer::Newer);
    EXPECT_EQ(beacon.supersededBy(), std::optional<uint64_t>(3)) << "generation 3 names another key";
  }
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({beaconKey, nextKey}).isOk());
    EXPECT_EQ(beacon.supersededBy(), std::optional<uint64_t>(3)) << "it stays down after a restart";
  }
  std::ofstream(certFile, std::ios::trunc) << "garbage";
  Beacon unreadable;
  EXPECT_FALSE(unreadable.mount(mount).isOk()) << "a certificate file that does not read is not ignored";
}

// beacon-authority ba6: equivocation evidence against the beacon's own generation halts it — no block
// above the evidence's height, across restarts — until a later generation is certified; then it
// resumes and the evidence file goes.
TEST(BeaconServerInitTest, AnEquivocationHaltsTheBeaconUntilALaterGeneration) {
  GenesisSetup setup("pp-ledger-beacon-equivocation-test");
  setup.writeInitConfig();
  const auto beaconKey = utl::mlDsaGenerate().value();
  const auto nextKey = utl::mlDsaGenerate().value();
  setup.writeKeyPair(beaconKey, "keys/beacon-signing");
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  auto tipBy = [](const utl::MlDsaKeyPair &key, const std::string &hash) {
    Client::SignedTip tip;
    tip.networkId = "test-net";
    tip.generation = 1;
    tip.height = 0;
    tip.blockHash = hash;
    tip.signature = utl::mlDsaSign(key.privateKey, tip.signingMessage()).value();
    return tip;
  };
  const auto evidence = Client::Equivocation::of(tipBy(beaconKey, "x"), tipBy(beaconKey, "y")).value();
  Beacon::MountConfig mount;
  mount.workDir = (setup.dir / "data").string();
  const auto file = setup.dir / "data" / "equivocation.dat";
  Ledger::ChainNode next;
  next.block.index = 1;
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({beaconKey, nextKey}).isOk());
    EXPECT_FALSE(beacon.offerEquivocation(Client::Equivocation::of(tipBy(beaconKey, "x"),
                                                                   tipBy(utl::mlDsaGenerate().value(), "y"))
                                              .value())
                     .isOk())
        << "one tip not signed by the beacon";
    EXPECT_FALSE(beacon.haltedAbove().has_value());
    ASSERT_EQ(beacon.offerEquivocation(evidence).value(), Chain::EquivocationOffer::Newer);
    EXPECT_EQ(beacon.haltedAbove(), std::optional<uint64_t>(0));
    EXPECT_TRUE(std::filesystem::exists(file));
    auto refused = beacon.addBlock(next);
    ASSERT_FALSE(refused.isOk());
    EXPECT_NE(refused.error().message.find("Halted"), std::string::npos) << refused.error().message;
  }
  {
    Beacon beacon; // restart: still halted
    ASSERT_TRUE(beacon.mount(mount).isOk());
    ASSERT_TRUE(beacon.openTipSigning({beaconKey, nextKey}).isOk());
    EXPECT_EQ(beacon.haltedAbove(), std::optional<uint64_t>(0)) << "kept across the restart";
    EXPECT_EQ(beacon.getEquivocation(), evidence);

    // The admins certify generation 2 (a key this beacon holds): it resumes.
    BeaconCertificate cert;
    cert.networkId = "test-net";
    cert.generation = 2;
    cert.publicKey = nextKey.publicKey;
    cert.cutoverHeight = BeaconCertificate::kNoCutover;
    Ledger::TxBeaconCert tx;
    tx.meta = cert.ltsToString();
    Ledger::Record record;
    record.type = Ledger::T_BEACON_CERT;
    record.data = utl::binaryPack(tx);
    for (int i = 0; i < 2; ++i) {
      record.signatures.push_back(utl::mlDsaSign(setup.genesis[i].privateKey, record.signingMessage("test-net")).value());
    }
    ASSERT_EQ(beacon.offerCertificate(record).value(), Chain::CertificateOffer::Newer);
    EXPECT_FALSE(beacon.haltedAbove().has_value()) << "resumed";
    EXPECT_FALSE(std::filesystem::exists(file)) << "the evidence is superseded";
  }
  {
    Beacon beacon;
    ASSERT_TRUE(beacon.mount(mount).isOk());
    EXPECT_FALSE(beacon.haltedAbove().has_value());
  }
}

// The evidence store waits while the chain holds no block, like the certificate store.
TEST(BeaconServerInitTest, EquivocationStoreWaitsForTheFirstBlock) {
  GenesisSetup setup("pp-ledger-equivocation-store-test");
  setup.writeInitConfig();
  const auto beaconKey = utl::mlDsaGenerate().value();
  setup.writeKeyPair(beaconKey, "keys/beacon-signing");
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  auto tipBy = [&](const std::string &hash) {
    Client::SignedTip tip;
    tip.networkId = "test-net";
    tip.generation = 1;
    tip.height = 0;
    tip.blockHash = hash;
    tip.signature = utl::mlDsaSign(beaconKey.privateKey, tip.signingMessage()).value();
    return tip;
  };
  const std::string file = (setup.dir / "equivocation.dat").string();
  std::ofstream(file, std::ios::binary | std::ios::trunc)
      << Client::Equivocation::of(tipBy("x"), tipBy("y")).value().ltsToString();
  {
    Chain empty;
    EquivocationStore store(empty, file);
    ASSERT_TRUE(store.load().isOk());
    EXPECT_FALSE(empty.equivocation().has_value());
    store.retryPending();
    EXPECT_TRUE(std::filesystem::exists(file)) << "kept until the chain can verify it";
  }
  {
    Chain chain;
    ASSERT_TRUE(chain.mountLedger((setup.dir / "data" / "ledger").string()).isOk());
    ASSERT_TRUE(chain.loadFromLedger(0).isOk());
    EquivocationStore store(chain, file);
    ASSERT_TRUE(store.load().isOk());
    EXPECT_TRUE(chain.equivocation().has_value());
  }
}

// The store under any node's chain: it writes what the chain keeps, offers it again after a
// restart, waits while the chain holds no block, and removes a certificate the chain will not keep.
TEST(BeaconServerInitTest, CertificateStoreKeepsWhatTheChainKeeps) {
  GenesisSetup setup("pp-ledger-certificate-store-test");
  setup.writeInitConfig();
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  auto certFor = [&](uint64_t generation) {
    BeaconCertificate cert;
    cert.networkId = "test-net";
    cert.generation = generation;
    cert.cutoverHeight = BeaconCertificate::kNoCutover;
    cert.publicKey = utl::mlDsaGenerate().value().publicKey;
    Ledger::TxBeaconCert tx;
    tx.meta = cert.ltsToString();
    Ledger::Record record;
    record.type = Ledger::T_BEACON_CERT;
    record.data = utl::binaryPack(tx);
    for (int i = 0; i < 2; ++i) {
      record.signatures.push_back(utl::mlDsaSign(setup.genesis[i].privateKey, record.signingMessage("test-net")).value());
    }
    return record;
  };
  const std::string file = (setup.dir / "beacon-cert.dat").string();
  auto loadedChain = [&](Chain &chain) {
    ASSERT_TRUE(chain.mountLedger((setup.dir / "data" / "ledger").string()).isOk());
    ASSERT_TRUE(chain.loadFromLedger(0).isOk());
  };
  const auto gen2 = certFor(2);
  {
    Chain chain;
    loadedChain(chain);
    CertificateStore store(chain, file);
    ASSERT_TRUE(store.load().isOk());
    EXPECT_FALSE(std::filesystem::exists(file));
    ASSERT_EQ(store.offer(gen2).value(), Chain::CertificateOffer::Newer);
    EXPECT_TRUE(std::filesystem::exists(file));
  }
  {
    Chain empty; // a node with no blocks cannot verify it yet: it waits, the file stays
    CertificateStore store(empty, file);
    ASSERT_TRUE(store.load().isOk());
    store.retryPending();
    EXPECT_FALSE(empty.seenCertificate().has_value());
    EXPECT_TRUE(std::filesystem::exists(file));
  }
  {
    Chain chain;
    loadedChain(chain);
    CertificateStore store(chain, file);
    ASSERT_TRUE(store.load().isOk());
    ASSERT_TRUE(chain.seenCertificate().has_value()) << "restored after a restart";
    EXPECT_EQ(chain.seenCertificateRecord()->data, gen2.data);
  }
  std::ofstream(file, std::ios::binary | std::ios::trunc) << utl::binaryPack(certFor(1));
  {
    Chain chain;
    loadedChain(chain);
    CertificateStore store(chain, file);
    ASSERT_TRUE(store.load().isOk());
    EXPECT_FALSE(chain.seenCertificate().has_value());
    EXPECT_FALSE(std::filesystem::exists(file)) << "not above the generation in force: removed";
  }
}

// beacon-authority bp3: `pp-beacon --promote` turns a relay's work directory into the beacon taking
// over, and refuses one that cannot take over — before it writes anything.
TEST(BeaconServerInitTest, PromotionRefusesADirectoryThatCannotTakeOver) {
  GenesisSetup setup("pp-ledger-beacon-promote-test");
  setup.writeInitConfig();
  const auto oldKey = utl::mlDsaGenerate().value();
  setup.writeKeyPair(oldKey, "keys/beacon-signing");
  ASSERT_TRUE(setup.init({setup.keyFile(setup.genesis[0], "g0"), setup.keyFile(setup.genesis[1], "g1")}).isOk());
  const auto config = setup.dir / "config.json";
  const std::string relayConfig =
      R"({"port": 18519, "beacon": "/ip4/127.0.0.1/udp/18517/adp/1.0.0/p2p/QmQxy7idP1HWWrhX6rzSxBp4Q94mKBzGGFjW3oCFgi69LZ"})";
  auto promote = [&]() {
    BeaconServer server;
    server.setPromote(true);
    auto run = server.run(setup.dir.string());
    EXPECT_FALSE(run.isOk()) << "refused";
    EXPECT_FALSE(std::filesystem::exists(setup.dir / "config.relay.json")) << "nothing written";
    return run.isOk() ? std::string{} : run.error().message;
  };
  auto expectRefused = [&](const std::string &why) {
    const std::string message = promote();
    EXPECT_NE(message.find(why), std::string::npos) << message;
  };
  auto certFor = [&](const utl::MlDsaKeyPair &key, uint64_t cutover) {
    Beacon reader;
    Beacon::MountConfig mounted;
    mounted.workDir = (setup.dir / "data").string();
    EXPECT_TRUE(reader.mount(mounted).isOk());
    BeaconCertificate cert;
    cert.networkId = "test-net";
    cert.generation = 2;
    cert.publicKey = key.publicKey;
    cert.cutoverHeight = cutover;
    cert.cutoverHash = cutover == BeaconCertificate::kNoCutover ? std::string() : reader.readBlock(0).value().hash;
    Ledger::TxBeaconCert tx;
    tx.meta = cert.ltsToString();
    tx.idempotentId = 1;
    tx.validationTsMin = 0;
    tx.validationTsMax = std::numeric_limits<int32_t>::max();
    Ledger::Record record;
    record.type = Ledger::T_BEACON_CERT;
    record.data = utl::binaryPack(tx);
    for (int i = 0; i < 2; ++i) {
      record.signatures.push_back(utl::mlDsaSign(setup.genesis[i].privateKey, record.signingMessage("test-net")).value());
    }
    EXPECT_EQ(reader.offerCertificate(record).value(), Chain::CertificateOffer::Newer); // kept on disk
  };

  std::ofstream(config) << R"({"port": 18517})";
  expectRefused("is not a relay's");

  std::ofstream(config) << relayConfig;
  std::filesystem::remove(setup.dir / "data" / "signed-tip.dat");
  expectRefused("no beacon certificate above generation 1 is held");

  const auto newKey = utl::mlDsaGenerate().value();
  certFor(utl::mlDsaGenerate().value(), 0);
  expectRefused("names another key");

  std::filesystem::remove(setup.dir / "data" / "beacon-cert.dat");
  setup.writeKeyPair(newKey, "keys/beacon-signing"); // the key the admins certified, put in place
  certFor(newKey, BeaconCertificate::kNoCutover);
  expectRefused("has no cutover");

  std::filesystem::remove(setup.dir / "data" / "beacon-cert.dat");
  certFor(newKey, 0);
  {
    Beacon signer; // a beacon's directory: it signed a tip
    Beacon::MountConfig mounted;
    mounted.workDir = (setup.dir / "data").string();
    ASSERT_TRUE(signer.mount(mounted).isOk());
    ASSERT_TRUE(signer.openTipSigning({oldKey}).isOk());
  }
  ASSERT_TRUE(std::filesystem::exists(setup.dir / "data" / "signed-tip.dat"));
  expectRefused("was a beacon's");
  EXPECT_EQ(readAllText(config), relayConfig) << "the relay's config is untouched";
}

// --- Relay / miner without config: a template to fill in, never a guess ---

#include "server/MinerServer.h"
#include "server/RelayServer.h"
#include "ledger/network/amp/AmpIdentity.h"

#include <random>
#include <thread>

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

// A miner waits at start for an upstream to answer — its relay may be restarting too (a rewind
// restarts every node at once) — and a stop request during the wait ends the start cleanly, not as
// a failure.
TEST(RunConfigTest, AMinerWaitingForItsUpstreamStopsCleanlyOnRequest) {
  GenesisSetup scratch("pp-ledger-miner-wait-upstream-test");
  const auto key = utl::mlDsaGenerate().value();
  std::ofstream(scratch.dir / "miner.key") << utl::hexEncode(key.privateKey) << "\n";
  const auto silent = network::LedgerAmpConfigFromPrivateKey(utl::mlDsaGenerate().value().privateKey, 0);
  ASSERT_TRUE(silent.isOk());
  const std::string upstream = "/ip4/127.0.0.1/udp/9/adp/1.0.0/p2p/" + silent.value().local_peer_id; // nobody there
  std::ofstream(scratch.dir / "config.json")
      << R"({"minerId": 1048576, "keys": ["miner.key"], "port": )" << 41000 + std::random_device{}() % 2000
      << R"(, "beacons": [")" << upstream << R"("]})";

  MinerServer server;
  std::thread stopper([&server]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    server.setStop(true); // what SIGTERM does
  });
  const auto started = std::chrono::steady_clock::now();
  auto run = server.run(scratch.dir.string());
  stopper.join();
  EXPECT_TRUE(run.isOk()) << run.error().message;
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(10)) << "the stop ends the wait";
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
    EXPECT_EQ(gm.config.finalitySeconds, 86400u) << "a day (projects/finality FN001)";
  }
  const std::pair<std::string, std::string> refused[] = {
      {R"(, "slotsPerEpoch": 100, "heartbeatSlots": 101)", "heartbeatSlots"},
      {R"(, "slotDuration": 5, "minerDueAfterSlots": 8639)", "minerDueAfterSlots must be 0 or at least 8640"},
      {R"(, "finalitySeconds": 600)", "finalitySeconds must be between 3600 and 2592000"},
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
