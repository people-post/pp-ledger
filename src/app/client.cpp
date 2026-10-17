#include "Client.h"
#include "AccountAttachment.h"
#include "AccountIds.h"
#include "LoadGenerator.h"
#include "../ledger/Ledger.h"
#include "../chain/BlockValidation.h"
#include "../chain/PredecessorCheck.h"
#include "../chain/StateSnapshot.h"
#include "../chain/Types.h"
#include "../consensus/Types.hpp"
#include "../network/LedgerAmpRuntime.h"
#include "../network/amp/AmpIdentity.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Crypto.h"
#include "common/Logger.h"
#include "lib/common/Utilities.h"
#include "common/io/Json.h"

#include <cli11.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

static constexpr uint64_t ID_GENESIS = pp::AccountIds::ID_GENESIS;
static constexpr uint64_t ID_FIRST_USER = pp::AccountIds::ID_FIRST_USER;

static pp::Client::UserAccount makeNewUserAccountMeta(const std::string& pubkeyHex,
                                                     uint64_t amount,
                                                     const std::string& metaDesc,
                                                     uint8_t minSignatures) {
  pp::Client::UserAccount account;
  std::string pk = pubkeyHex;
  if (pk.size() >= 2 && (pk[0] == '0' && (pk[1] == 'x' || pk[1] == 'X')))
    pk = pk.substr(2);
  std::string decoded = pp::utl::hexDecode(pk);
  if (decoded.size() != pp::utl::kMlDsaPublicKeyBytes) {
    return account;  // Caller should validate
  }
  account.wallet.publicKeys.push_back(decoded);
  account.wallet.minSignatures = minSignatures;
  account.wallet.keyType = pp::Crypto::TK_ML_DSA_65;
  account.wallet.mBalances[ID_GENESIS] = static_cast<int64_t>(amount);
  // Hard-cut: UserAccount.meta is AccountAttachment envelope (ignore free-text).
  (void)metaDesc;
  account.meta = pp::AccountAttachment::emptySerialized();
  return account;
}

static uint64_t randomAccountId() {
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<uint64_t> dist(ID_FIRST_USER, UINT64_MAX);
  return dist(gen);
}

/** Set idempotency and validation window on a user transaction (T_DEFAULT, T_NEW_USER, etc.). */
static void setValidationWindow(uint64_t& idempotentId, int64_t& validationTsMin,
                                int64_t& validationTsMax) {
  const int64_t now = static_cast<int64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  idempotentId = static_cast<uint64_t>(now) ^ (randomAccountId() & 0xFFFFULL);
  if (idempotentId == 0) idempotentId = 1;
  validationTsMin = now - 60;
  validationTsMax = now + 3600;
}

void printBeaconStatus(const pp::Client::BeaconState& status) {
  std::cout << "Current Timestamp: " << pp::utl::formatTimestampLocal(status.currentTimestamp) << "\n";
  if (status.signedTip) {
    std::cout << "Signed tip: block " << status.signedTip->height << ", generation " << status.signedTip->generation
              << "\n";
  } else {
    std::cout << "Signed tip: none\n";
  }
  if (status.certificate) {
    auto tx = pp::utl::binaryUnpack<pp::Ledger::TxBeaconCert>(status.certificate->data);
    pp::BeaconCertificate cert;
    if (tx && cert.ltsFromString(tx.value().meta)) {
      std::cout << "Certificate: generation " << cert.generation << ", cutover "
                << (cert.cutoverHeight == UINT64_MAX ? std::string("none") : std::to_string(cert.cutoverHeight))
                << "\n";
      for (const auto &hint : cert.dialHints) {
        std::cout << "Certificate dial hint: " << hint << "\n";
      }
    } else {
      std::cout << "Certificate: unreadable\n";
    }
  }
  if (status.recovery) {
    std::cout << "Recovery: pending, applies at epoch " << status.recovery->applyEpoch << " (announced at block "
              << status.recovery->announcedAt << "; new admin set: " << status.recovery->adminPublicKeys.size()
              << " keys, " << static_cast<int>(status.recovery->adminMinSignatures)
              << " signatures; the admin set is frozen until then)\n";
  }
  if (status.rewound) {
    std::cout << "Rewound: kept blocks up to " << status.rewound->keptUpTo << ", dropped "
              << status.rewound->keptUpTo + 1 << " to " << status.rewound->droppedUpTo
              << " (beacon generation " << status.rewound->generation << " cutover)\n";
  }
  if (status.equivocation) {
    std::cout << "Equivocation: generation " << status.equivocation->generation() << " at block "
              << status.equivocation->height() << " (halted above it)\n";
  }
  std::cout << pp::common::io::metaToJsonString(status.ltsToMeta(), 2) << "\n";
}

/** Sign `payload` as a record of `type` with `key` (hex or file) and submit it. */
static int signAndSubmit(pp::Client& client, uint16_t type, std::string payload, const std::string& key) {
  auto keyResult = pp::utl::readPrivateKey(key, "");
  if (!keyResult) {
    std::cerr << "Error: --key: " << keyResult.error().message << "\n";
    return 1;
  }
  const std::string privateKey = keyResult.value();
  pp::Ledger::Record rec;
  rec.type = type;
  rec.data = std::move(payload);
  std::string networkId;
  if (auto st = client.fetchBeaconState()) {
    networkId = st->networkId;
  }
  auto sigResult =
      pp::utl::mlDsaSign(privateKey, rec.signingMessage(networkId));
  if (!sigResult) {
    std::cerr << "Error: " << sigResult.error().message << "\n";
    return 1;
  }
  rec.signatures = {*sigResult};
  auto result = client.submitTransaction(rec); // the same bytes again on overload (never re-signed)
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction submitted successfully\n";
  return 0;
}

static int runAddTx(pp::Client& client, uint64_t fromWalletId, uint64_t toWalletId,
                    uint64_t amount, uint64_t fee, const std::string& key, uint64_t idempotentId) {
  pp::Ledger::TxDefault tx;
  tx.fromWalletId = fromWalletId;
  tx.toWalletId = toWalletId;
  tx.amount = amount;
  tx.fee = fee;
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  if (idempotentId != 0) {
    tx.idempotentId = idempotentId; // run again with the same id, the chain takes it once
  }
  std::cout << "Idempotent id: " << tx.idempotentId
            << " (to run this again safely, pass --idempotent-id " << tx.idempotentId << ")\n";
  return signAndSubmit(client, pp::Ledger::T_DEFAULT, pp::utl::binaryPack(tx), key);
}

/** T_MINER_REGISTER / T_MINER_REVOKE, signed by the account itself. */
static int runMinerTx(pp::Client& client, uint16_t type, uint64_t walletId, uint64_t fee,
                      const std::string& key) {
  pp::Ledger::TxMiner tx;
  tx.walletId = walletId;
  tx.fee = fee;
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  return signAndSubmit(client, type, pp::utl::binaryPack(tx), key);
}

using TxRecord = pp::Ledger::Record;

static pp::Roe<std::string> readFileContent(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return pp::Error(1, "Cannot open file: " + path);
  std::ostringstream oss;
  oss << f.rdbuf();
  if (!f)
    return pp::Error(2, "Failed to read file: " + path);
  return oss.str();
}

static int runMkTx(uint64_t fromWalletId, uint64_t toWalletId, uint64_t amount,
                   const std::string& outputPath) {
  pp::Ledger::TxDefault tx;
  tx.fromWalletId = fromWalletId;
  tx.toWalletId = toWalletId;
  tx.amount = amount;
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  TxRecord rec;
  rec.type = pp::Ledger::T_DEFAULT;
  rec.data = pp::utl::binaryPack(tx);
  rec.signatures = {};
  std::string packed = pp::utl::binaryPack(rec);
  auto result = pp::utl::writeToNewFile(outputPath, packed);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction written to " << outputPath << "\n";
  return 0;
}

/**
 * mk-equivocation (beacon-authority ba6): equivocation evidence signed with a beacon key — two tips
 * at one height for two made-up blocks. For drills and tests: it halts every node that takes it
 * until the admins certify a later generation. Submit with add-equivocation.
 */
static int runMkEquivocation(const std::string &networkId, uint64_t generation, uint64_t height,
                             const std::string &key, const std::string &outputPath) {
  auto keyResult = pp::utl::readPrivateKey(key, "");
  if (!keyResult) {
    std::cerr << "Error: --key: " << keyResult.error().message << "\n";
    return 1;
  }
  auto tipFor = [&](const std::string &label) -> std::optional<pp::Client::SignedTip> {
    pp::Client::SignedTip tip;
    tip.networkId = networkId;
    tip.generation = generation;
    tip.height = height;
    tip.blockHash = pp::utl::sha256Raw("pp-ledger/equivocation-drill/" + label + std::to_string(randomAccountId()));
    auto signature = pp::utl::mlDsaSign(keyResult.value(), tip.signingMessage());
    if (!signature) {
      std::cerr << "Error: " << signature.error().message << "\n";
      return std::nullopt;
    }
    tip.signature = signature.value();
    return tip;
  };
  const auto a = tipFor("a");
  const auto b = tipFor("b");
  if (!a || !b) {
    return 1;
  }
  auto result = pp::utl::writeToNewFile(outputPath, pp::Client::Equivocation::of(*a, *b)->ltsToString());
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Equivocation evidence (generation " << generation << ", block " << height << ") written to "
            << outputPath << "\n";
  return 0;
}

/** add-equivocation (ba6): push evidence from a file to a node, which keeps it and passes it up. */
static int runAddEquivocation(pp::Client &client, const std::string &filePath) {
  auto content = readFileContent(filePath);
  if (!content) {
    std::cerr << "Error: " << content.error().message << "\n";
    return 1;
  }
  pp::Client::Equivocation evidence;
  if (!evidence.ltsFromString(content.value())) {
    std::cerr << "Error: " << filePath << " is not equivocation evidence\n";
    return 1;
  }
  auto result = client.addEquivocation(evidence);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << result.value() << "\n";
  return 0;
}

/** An ML-DSA-65 public key given as hex or as a .pub file; empty if it is not one. */
static std::string readPublicKeyArg(const std::string &arg) {
  std::string hex = arg;
  if (std::ifstream pubFile(arg); pubFile) {
    std::ostringstream text;
    text << pubFile.rdbuf();
    hex = text.str();
  }
  hex.erase(std::remove_if(hex.begin(), hex.end(), [](unsigned char c) { return std::isspace(c); }), hex.end());
  if (hex.rfind("0x", 0) == 0) {
    hex = hex.substr(2);
  }
  std::string key = pp::utl::hexDecode(hex);
  return key.size() == pp::utl::kMlDsaPublicKeyBytes ? key : std::string{};
}

/**
 * mk-recovery (beacon-authority ba7): an unsigned T_RECOVERY announcing a new admin set. The
 * recovery key holders add their signatures with sign-tx (--network-id) and submit with submit-tx;
 * it applies `delayEpochs` after it commits, and freezes the admin set until then.
 */
static int runMkRecovery(const std::string &networkId, const std::vector<std::string> &adminKeyArgs,
                         unsigned minSignatures, const std::string &outputPath) {
  pp::RecoveryAnnouncement announcement;
  announcement.networkId = networkId;
  for (const auto &arg : adminKeyArgs) {
    std::string key = readPublicKeyArg(arg);
    if (key.empty()) {
      std::cerr << "Error: --admin-pubkey " << arg << " is not an ML-DSA-65 public key (hex or a .pub file)\n";
      return 1;
    }
    announcement.adminPublicKeys.push_back(std::move(key));
  }
  if (minSignatures < 2 || minSignatures > announcement.adminPublicKeys.size() || minSignatures > 255) {
    std::cerr << "Error: --min-signatures must be between 2 and the number of admin keys\n";
    return 1;
  }
  announcement.adminMinSignatures = static_cast<uint8_t>(minSignatures);
  pp::Ledger::TxRecovery tx;
  tx.fee = 0;
  tx.meta = announcement.ltsToString();
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  TxRecord rec;
  rec.type = pp::Ledger::T_RECOVERY;
  rec.data = pp::utl::binaryPack(tx);
  auto result = pp::utl::writeToNewFile(outputPath, pp::utl::binaryPack(rec));
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Recovery announcement (" << announcement.adminPublicKeys.size() << " admin keys, "
            << minSignatures << " signatures) written to " << outputPath
            << ": sign with the recovery keys (sign-tx), then submit-tx\n";
  return 0;
}

/**
 * With --cutover (beacon-authority bp2): read block `cutover` from the node the client is connected
 * to, return its hash — computed from the block, not taken from the node — and print how much the
 * cut drops, so the admins see it before they sign.
 */
static pp::Roe<std::string> describeCutover(pp::Client &client, uint64_t cutover) {
  auto status = client.fetchBeaconState();
  if (!status) {
    return pp::Error(1, "Failed to read the node's status: " + status.error().message);
  }
  const uint64_t next = status.value().nextBlockId;
  if (cutover >= next) {
    return pp::Error(1, "Block " + std::to_string(cutover) + " does not exist on this node (next block " +
                            std::to_string(next) + "): a cutover names a block the old key already signed. An "
                            "honest rotation needs no cutover");
  }
  auto block = client.fetchBlock(cutover);
  auto last = client.fetchBlock(next - 1);
  if (!block || !last) {
    return pp::Error(1, "Failed to read blocks " + std::to_string(cutover) + " and " + std::to_string(next - 1));
  }
  const std::string hash = pp::chain_block::calculateBlockHash(block.value().block);
  const int64_t seconds = last.value().block.timestamp - block.value().block.timestamp;
  std::cout << "Cutover at block " << cutover << " (hash 0x" << pp::utl::hexEncode(hash) << ").\n"
            << "Every node holding them drops blocks " << cutover + 1 << " to " << next - 1 << ": "
            << next - 1 - cutover << " blocks, about " << seconds / 3600 << " h of chain time on this node.\n"
            << "Nodes rewind on their own up to 7 days of slots back; deeper, each operator must approve "
            << "(--approve-rewind).\n";
  return hash;
}

/**
 * mk-beacon-cert (beacon-authority ba2): an unsigned T_BEACON_CERT record for the next beacon
 * generation. Admins add their signatures with sign-tx (--network-id) and submit with submit-tx.
 */
static int runMkBeaconCert(const std::string &networkId, uint64_t generation, const std::string &pubkeyArg,
                           uint64_t cutoverHeight, const std::string &cutoverHash,
                           const std::vector<std::string> &dialHints, const std::string &outputPath) {
  pp::BeaconCertificate cert;
  cert.networkId = networkId;
  cert.generation = generation;
  cert.publicKey = readPublicKeyArg(pubkeyArg);
  cert.cutoverHeight = cutoverHeight;
  cert.cutoverHash = cutoverHash;
  cert.dialHints = dialHints;
  if (cert.publicKey.empty()) {
    std::cerr << "Error: --pubkey must be an ML-DSA-65 public key (hex or a .pub file)\n";
    return 1;
  }
  pp::Ledger::TxBeaconCert tx;
  tx.fee = 0;
  tx.meta = cert.ltsToString();
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  TxRecord rec;
  rec.type = pp::Ledger::T_BEACON_CERT;
  rec.data = pp::utl::binaryPack(tx);
  auto result = pp::utl::writeToNewFile(outputPath, pp::utl::binaryPack(rec));
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Beacon certificate (generation " << generation << ") written to " << outputPath
            << "; sign it with the admin keys (sign-tx --network-id " << networkId << ") and submit-tx\n";
  return 0;
}

static int runMkAccount(uint64_t fromWalletId, uint64_t toWalletId, uint64_t amount,
                       uint64_t fee, const std::string& newPubkeyHex,
                       const std::string& metaDesc, uint8_t minSignatures,
                       const std::string& outputPath, bool toWasGenerated) {
  std::string pubkeyToUse;
  std::string privateKeyToPrint;  // Non-empty when key was auto-generated
  if (!newPubkeyHex.empty()) {
    auto userAccount = makeNewUserAccountMeta(newPubkeyHex, amount, metaDesc, minSignatures);
    if (userAccount.wallet.publicKeys.empty()) {
      std::cerr << "Error: --new-pubkey must be ML-DSA-65 public key (1952 bytes / 3904 hex chars).\n";
      return 1;
    }
    pubkeyToUse = userAccount.wallet.publicKeys[0];
  } else {
    auto pair = pp::utl::mlDsaGenerate();
    if (!pair.isOk()) {
      std::cerr << "Error: " << pair.error().message << "\n";
      return 1;
    }
    pubkeyToUse = pair->publicKey;
    privateKeyToPrint = pp::utl::hexEncode(pair->privateKey);
  }
  pp::Client::UserAccount userAccount;
  userAccount.wallet.publicKeys.push_back(pubkeyToUse);
  userAccount.wallet.minSignatures = minSignatures;
  userAccount.wallet.keyType = pp::Crypto::TK_ML_DSA_65;
  userAccount.wallet.mBalances[ID_GENESIS] = static_cast<int64_t>(amount);
  (void)metaDesc;
  userAccount.meta = pp::AccountAttachment::emptySerialized();
  pp::Ledger::TxNewUser tx;
  tx.fromWalletId = fromWalletId;
  tx.toWalletId = toWalletId;
  tx.amount = amount;
  tx.fee = fee;
  tx.meta = userAccount.ltsToString();
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  TxRecord rec;
  rec.type = pp::Ledger::T_NEW_USER;
  rec.data = pp::utl::binaryPack(tx);
  rec.signatures = {};
  std::string packed = pp::utl::binaryPack(rec);
  auto result = pp::utl::writeToNewFile(outputPath, packed);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "T_NEW_USER transaction written to " << outputPath << "\n";
  std::cout << "  New account ID:    " << toWalletId;
  if (toWasGenerated) std::cout << " (randomly generated - save this ID)";
  std::cout << "\n";
  if (!privateKeyToPrint.empty()) {
    std::cout << "\nGenerated new key pair. Save the private key securely:\n";
    std::cout << "  Private key (hex): " << privateKeyToPrint << "\n";
    std::cout << "  Public key (hex):  " << pp::utl::hexEncode(pubkeyToUse) << "\n";
  }
  return 0;
}

static int runAddAccount(pp::Client& client, uint64_t fromWalletId, uint64_t toWalletId,
                         uint64_t amount, uint64_t fee, const std::string& newPubkeyHex,
                         const std::string& metaDesc, uint8_t minSignatures,
                         const std::string& key, bool toWasGenerated) {
  std::string pubkeyToUse;
  std::string privateKeyToPrint;  // Non-empty when key was auto-generated
  if (!newPubkeyHex.empty()) {
    auto userAccount = makeNewUserAccountMeta(newPubkeyHex, amount, metaDesc, minSignatures);
    if (userAccount.wallet.publicKeys.empty()) {
      std::cerr << "Error: --new-pubkey must be ML-DSA-65 public key (1952 bytes / 3904 hex chars).\n";
      return 1;
    }
    pubkeyToUse = userAccount.wallet.publicKeys[0];
  } else {
    auto pair = pp::utl::mlDsaGenerate();
    if (!pair.isOk()) {
      std::cerr << "Error: " << pair.error().message << "\n";
      return 1;
    }
    pubkeyToUse = pair->publicKey;
    privateKeyToPrint = pp::utl::hexEncode(pair->privateKey);
  }
  pp::Client::UserAccount userAccount;
  userAccount.wallet.publicKeys.push_back(pubkeyToUse);
  userAccount.wallet.minSignatures = minSignatures;
  userAccount.wallet.keyType = pp::Crypto::TK_ML_DSA_65;
  userAccount.wallet.mBalances[ID_GENESIS] = static_cast<int64_t>(amount);
  (void)metaDesc;
  userAccount.meta = pp::AccountAttachment::emptySerialized();
  auto keyResult = pp::utl::readPrivateKey(key, "");
  if (!keyResult) {
    std::cerr << "Error: --key: " << keyResult.error().message << "\n";
    return 1;
  }
  const std::string privateKey = keyResult.value();
  pp::Ledger::TxNewUser tx;
  tx.fromWalletId = fromWalletId;
  tx.toWalletId = toWalletId;
  tx.amount = amount;
  tx.fee = fee;
  tx.meta = userAccount.ltsToString();
  setValidationWindow(tx.idempotentId, tx.validationTsMin, tx.validationTsMax);
  std::string payload = pp::utl::binaryPack(tx);
  TxRecord rec;
  rec.type = pp::Ledger::T_NEW_USER;
  rec.data = std::move(payload);
  std::string networkId;
  if (auto st = client.fetchBeaconState()) {
    networkId = st->networkId;
  }
  auto sigResult =
      pp::utl::mlDsaSign(privateKey, rec.signingMessage(networkId));
  if (!sigResult) {
    std::cerr << "Error: " << sigResult.error().message << "\n";
    return 1;
  }
  rec.signatures = {*sigResult};
  auto result = client.submitTransaction(rec); // the same bytes again on overload (never re-signed)
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Account creation transaction submitted successfully\n";
  std::cout << "  New account ID:    " << toWalletId;
  if (toWasGenerated) std::cout << " (randomly generated - save this ID)";
  std::cout << "\n";
  if (!privateKeyToPrint.empty()) {
    std::cout << "\nGenerated new key pair. Save the private key securely:\n";
    std::cout << "  Private key (hex): " << privateKeyToPrint << "\n";
    std::cout << "  Public key (hex):  " << pp::utl::hexEncode(pubkeyToUse) << "\n";
  }
  return 0;
}

static int runSignTx(const std::string& filePath, const std::string& key,
                     const std::string& networkId) {
  auto content = readFileContent(filePath);
  if (!content) {
    std::cerr << "Error: " << content.error().message << "\n";
    return 1;
  }
  auto recResult = pp::utl::binaryUnpack<TxRecord>(*content);
  if (!recResult) {
    std::cerr << "Error: Invalid tx record file: " << recResult.error().message << "\n";
    return 1;
  }
  TxRecord rec = *recResult;
  auto keyResult = pp::utl::readPrivateKey(key, "");
  if (!keyResult) {
    std::cerr << "Error: --key: " << keyResult.error().message << "\n";
    return 1;
  }
  const std::string privateKey = keyResult.value();
  std::string message = rec.signingMessage(networkId);
  auto sigResult = pp::utl::mlDsaSign(privateKey, message);
  if (!sigResult) {
    std::cerr << "Error: " << sigResult.error().message << "\n";
    return 1;
  }
  rec.signatures.push_back(*sigResult);
  std::string packed = pp::utl::binaryPack(rec);
  std::ofstream f(filePath, std::ios::binary | std::ios::trunc);
  if (!f) {
    std::cerr << "Error: Cannot write file: " << filePath << "\n";
    return 1;
  }
  f << packed;
  if (!f) {
    std::cerr << "Error: Failed to write file: " << filePath << "\n";
    return 1;
  }
  std::cout << "Added signature (" << rec.signatures.size() << " total).\n";
  return 0;
}

/** `health`: the node's HEALTH as JSON; exit 0 when live (and ready, with --ready). */
/**
 * check-predecessor: block 0 of the network `client` talks to must be a re-genesis naming block H of
 * the old ledger in `oldDir` (beacon-authority B037) — the check relays and miners run with
 * networkAnchor.predecessorDir, by hand.
 */
static int runCheckPredecessor(pp::Client& client, const std::string& oldDir) {
  auto genesis = client.fetchBlock(0);
  if (!genesis) {
    std::cerr << "Error: cannot fetch block 0 of the new network: " << genesis.error().message << "\n";
    return 1;
  }
  // The block as served, its hash computed here: not the node's word for it.
  const std::string genesisHash = pp::chain_block::calculateBlockHash(genesis.value().block);
  if (auto checked = pp::predecessor_check::check(genesis.value(), oldDir); !checked) {
    std::cerr << "Refused: " << checked.error().message << "\n";
    return 1;
  }
  auto tx = pp::utl::binaryUnpack<pp::Ledger::TxGenesis>(genesis.value().block.records.front().data);
  pp::GenesisAccountMeta gm;
  if (!tx || !gm.ltsFromString(tx.value().meta)) {
    std::cerr << "Error: cannot read the genesis meta\n";
    return 1;
  }
  const auto& p = gm.import.predecessor;
  std::cout << "Network '" << gm.config.networkId << "', genesis " << pp::utl::hexEncode(genesisHash) << "\n"
            << "  imports " << gm.import.accountCount << " accounts of network '" << p.networkId << "' at block "
            << p.height << "\n"
            << "  block hash " << pp::utl::hexEncode(p.blockHash) << "\n"
            << "Matches block " << p.height << " of the old ledger in " << oldDir << ".\n"
            << "Compare the genesis hash with the announcement, then pin it (networkAnchor.genesisHash).\n";
  return 0;
}

static int runHealth(pp::Client& client, bool requireReady) {
  auto result = client.fetchHealth();
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  const auto& health = result.value();
  std::cout << pp::common::io::metaToJsonString(health.ltsToMeta(), 2) << "\n";
  if (!health.live()) {
    std::cerr << "Not live: the server thread has not reported for " << health.reportAgeMs << " ms\n";
    return 1;
  }
  if (requireReady && !health.ready) {
    std::cerr << "Not ready: " << health.notReady << "\n";
    return 1;
  }
  return 0;
}

/** export-state: the snapshot of `dir`'s ledger after block `height`, written to `out`. */
static int runExportState(const std::string& dir, uint64_t height, const std::string& out, const std::string& scratch) {
  if (std::filesystem::exists(out)) {
    std::cerr << "Error: " << out << " exists (a snapshot is never overwritten)\n";
    return 1;
  }
  const std::string ledgerDir = (std::filesystem::path(dir) / "data" / "ledger").string();
  auto snapshot =
      pp::state_export::exportState(ledgerDir, height, scratch.empty() ? out + ".scratch" : scratch);
  if (!snapshot) {
    std::cerr << "Error: " << snapshot.error().message << "\n";
    return 1;
  }
  const auto& s = snapshot.value();
  if (auto written = pp::utl::writeToNewFile(out, s.ltsToString()); !written) {
    std::cerr << "Error: " << out << ": " << written.error().message << "\n";
    return 1;
  }
  std::cout << "Wrote " << out << ": " << s.accounts.size() << " accounts of network '" << s.predecessor.networkId
            << "' at block " << s.predecessor.height << "\n"
            << "  block hash: " << pp::utl::hexEncode(s.predecessor.blockHash) << "\n"
            << "  state root: " << pp::utl::hexEncode(s.predecessor.stateRoot) << " (checked)\n"
            << "Announce the network id, height and block hash with the new genesis: anyone holding the old\n"
            << "chain checks them against their own block " << s.predecessor.height << ".\n";
  return 0;
}

static int runSubmitTx(pp::Client& client, const std::string& filePath) {
  auto content = readFileContent(filePath);
  if (!content) {
    std::cerr << "Error: " << content.error().message << "\n";
    return 1;
  }
  auto recResult = pp::utl::binaryUnpack<TxRecord>(*content);
  if (!recResult) {
    std::cerr << "Error: Invalid tx record file: " << recResult.error().message << "\n";
    return 1;
  }
  auto result = client.submitTransaction(*recResult);
  if (!result) {
    std::cerr << "Error: " << result.error().message << "\n";
    return 1;
  }
  std::cout << "Transaction submitted successfully\n";
  return 0;
}

static int runLoadSetup(const std::string& host, pp::load::SetupConfig cfg, uint64_t timeoutSeconds,
                        const std::string& out) {
  if (std::filesystem::exists(out)) {
    std::cerr << "Error: " << out << " exists\n";
    return 1;
  }
  cfg.host = host;
  cfg.timeout = std::chrono::seconds(timeoutSeconds);
  for (auto& key : cfg.fromKeys) {
    auto raw = pp::utl::readPrivateKey(key, "");
    if (!raw) {
      std::cerr << "Error: --key: " << raw.error().message << "\n";
      return 1;
    }
    key = raw.value();
  }
  std::string networkId;
  auto accounts = pp::load::setup(cfg, networkId);
  if (!accounts) {
    std::cerr << "Error: " << accounts.error().message << "\n";
    return 1;
  }
  if (auto written = pp::load::writeAccounts(out, networkId, accounts.value()); !written) {
    std::cerr << "Error: " << written.error().message << "\n";
    return 1;
  }
  std::cout << "Created " << accounts.value().size() << " of " << cfg.accounts << " accounts (network " << networkId
            << "): " << out << "\n";
  return accounts.value().size() == cfg.accounts ? 0 : 1;
}

static int runLoad(const std::string& host, pp::load::RunConfig cfg, const std::string& accountsFile,
                   const std::string& mix, uint64_t durationSeconds, uint64_t burstAtSeconds, uint64_t drainSeconds,
                   const std::string& jsonOut) {
  auto parsed = pp::load::parseMix(mix);
  if (!parsed) {
    std::cerr << "Error: --mix: " << parsed.error().message << "\n";
    return 1;
  }
  cfg.host = host;
  cfg.mix = parsed.value();
  cfg.duration = std::chrono::seconds(durationSeconds);
  cfg.burstAt = std::chrono::seconds(burstAtSeconds);
  cfg.drain = std::chrono::seconds(drainSeconds);
  std::vector<pp::load::Account> accounts;
  if (!accountsFile.empty()) {
    std::string networkId;
    auto read = pp::load::readAccounts(accountsFile, networkId);
    if (!read) {
      std::cerr << "Error: " << read.error().message << "\n";
      return 1;
    }
    accounts = std::move(read.value());
  }
  auto report = pp::load::run(cfg, accounts);
  if (!report) {
    std::cerr << "Error: " << report.error().message << "\n";
    return 1;
  }
  std::cout << report.value().text();
  if (!jsonOut.empty()) {
    auto text = pp::common::io::valueToJsonString(
        pp::common::Value(std::make_shared<pp::common::Object>(report.value().json())), 2);
    std::ofstream out(jsonOut, std::ios::trunc);
    out << (text.isOk() ? text.value() : std::string("{}")) << "\n";
  }
  return 0;
}

int main(int argc, char *argv[]) {
  CLI::App app{"pp-client - Command-line client for pp-ledger beacon and miner servers"};
  app.require_subcommand(1);

  // Global options
  bool debug = false;
  app.add_flag("--debug", debug, "Enable debug logging");

  std::string host = pp::Client::DEFAULT_HOST;
  app.add_option("--host", host, "Server host (or host:port)")
      ->capture_default_str();

  std::string hostFile;
  app.add_option("--host-file", hostFile,
                 "Read the server multiaddr from a file (a node writes <work dir>/listen.multiaddr)");

  uint16_t port = 0;
  app.add_option("-p,--port", port, "Server port (overrides default)")
      ->check(CLI::Range(1, 65535));

  bool connectToBeacon = false;
  app.add_flag("-b,--beacon", connectToBeacon, "Connect to BeaconServer (default port: 8517)");

  bool connectToMiner = false;
  app.add_flag("-m,--miner", connectToMiner, "Connect to MinerServer (default port: 8518)");

  // Local command: keygen
  auto* keygen = app.add_subcommand("keygen", "Generate a new ML-DSA-65 key pair");
  std::string keygenOut;
  keygen->add_option("-o,--out", keygenOut,
                     "Write <name>.key (private, hex, 0600) and <name>.pub (public, hex); never overwrites");

  // Local command: export-state (re-genesis, beacon-authority B033)
  auto* export_state_cmd = app.add_subcommand(
      "export-state", "Write the account state of a node's ledger at a height, for a re-genesis to import");
  std::string exportDir;
  uint64_t exportHeight = 0;
  std::string exportOut;
  std::string exportScratch;
  export_state_cmd->add_option("-d,--dir", exportDir, "A full node's work directory (a relay's: from genesis); left as it is")
      ->required();
  export_state_cmd->add_option("--height", exportHeight, "The last block whose state is exported")->required();
  export_state_cmd->add_option("-o,--out", exportOut, "Snapshot file to write (never overwritten)")->required();
  export_state_cmd->add_option("--scratch", exportScratch,
                               "Where to copy the ledger while exporting: a path that does not exist (default: <out>.scratch; removed after)");

  // Local command: peer-id
  auto* peer_id_cmd = app.add_subcommand(
      "peer-id", "Print the network peer id and public key of a private key (to write multiaddrs ahead of start)");
  std::string peerIdKey;
  peer_id_cmd->add_option("key", peerIdKey, "Private key: hex, or a key file (e.g. keys/amp-identity.txt)")
      ->required();

  // Any node: health (a container health check: exit 0 when live, with --ready when ready)
  // Any node of a new network: check its re-genesis against an old ledger (beacon-authority B037)
  auto* check_predecessor_cmd = app.add_subcommand(
      "check-predecessor",
      "Check that the network --host belongs to is a re-genesis of block H of an old ledger; exit 1 if not");
  std::string predecessorDir;
  check_predecessor_cmd->add_option("-d,--dir", predecessorDir, "Your node's work directory on the old network")
      ->required();

  auto* health_cmd = app.add_subcommand("health", "Print a node's health (any role); exit 1 when not live");
  bool healthReady = false;
  health_cmd->add_flag("--ready", healthReady, "Exit 1 unless the node is also ready (synced, not halted)");

  // Beacon commands
  auto* beacon_status = app.add_subcommand("status", "Get beacon/miner status");

  auto* block_cmd = app.add_subcommand("block", "Get block by ID");
  uint64_t blockId = 0;
  block_cmd->add_option("blockId", blockId, "Block ID")->required();

  auto* account_cmd = app.add_subcommand("account", "Get account info by ID");
  uint64_t accountId = 0;
  account_cmd->add_option("accountId", accountId, "Account ID")->required();

  auto* txs_cmd = app.add_subcommand("transactions",
                                    "Get transactions by wallet ID (use -b/--beacon or -m/--miner)");
  uint64_t txs_walletId = 0;
  uint64_t txs_beforeBlockId = 0;
  txs_cmd->add_option("walletId", txs_walletId, "Wallet ID to query")->required();
  txs_cmd->add_option("--before", txs_beforeBlockId,
                     "Search backwards from before this block ID (0 = latest)")
      ->default_val(0);

  auto* tx_cmd = app.add_subcommand("transaction",
                                    "Get transaction by chain index (use -b/--beacon or -m/--miner)");
  uint64_t tx_index = 0;
  tx_cmd->add_option("index", tx_index, "Transaction index (0-based)")->required();

  // Miner commands
  auto* add_tx_cmd = app.add_subcommand("add-tx", "Submit a transaction (to a relay or the beacon, -b)");
  uint64_t fromWalletId = 0, toWalletId = 0;
  uint64_t amount = 0;
  uint64_t fee = 0;
  std::string key;
  add_tx_cmd->add_option("from", fromWalletId, "From wallet ID")->required();
  add_tx_cmd->add_option("to", toWalletId, "To wallet ID")->required();
  add_tx_cmd->add_option("amount", amount, "Amount to transfer")->required();
  add_tx_cmd->add_option("-f,--fee", fee, "Transaction fee (default: 0)")
      ->default_val(0);
  add_tx_cmd->add_option("-k,--key", key, "Private key (hex or file) to sign the transaction")
      ->required();
  uint64_t addTxIdempotentId = 0;
  add_tx_cmd->add_option("--idempotent-id", addTxIdempotentId,
                         "Idempotent id (default: random). The same id again is taken once: a safe re-run");

  // miner-register / miner-revoke: the account opts in or out of mining (from the next epoch)
  auto* miner_register_cmd = app.add_subcommand(
      "miner-register",
      "Register an account as a miner (fee: meta fee + minerRegistrationFee; relay or beacon, -b)");
  auto* miner_revoke_cmd =
      app.add_subcommand("miner-revoke", "Revoke an account's miner registration (relay or beacon, -b)");
  uint64_t minerWalletId = 0;
  uint64_t minerFee = 0;
  std::string minerKey;
  for (auto* cmd : {miner_register_cmd, miner_revoke_cmd}) {
    cmd->add_option("account", minerWalletId, "Miner account ID")->required();
    cmd->add_option("-f,--fee", minerFee, "Transaction fee")->required();
    cmd->add_option("-k,--key", minerKey, "The account's private key (hex or file)")->required();
  }

  // mk-tx: create unsigned SignedData and save to file
  auto* mk_tx_cmd = app.add_subcommand("mk-tx", "Create unsigned transaction file");
  uint64_t mk_from = 0, mk_to = 0;
  uint64_t mk_amount = 0;
  std::string mk_output;
  mk_tx_cmd->add_option("from", mk_from, "From wallet ID")->required();
  mk_tx_cmd->add_option("to", mk_to, "To wallet ID")->required();
  mk_tx_cmd->add_option("amount", mk_amount, "Amount to transfer")->required();
  mk_tx_cmd->add_option("-o,--output", mk_output, "Output file (must not exist)")
      ->required();

  // mk-beacon-cert: create an unsigned T_BEACON_CERT record file
  auto* mk_cert_cmd = app.add_subcommand(
      "mk-beacon-cert", "Create an unsigned beacon certificate (next generation) for the admin keys to sign");
  std::string mk_cert_network, mk_cert_pubkey, mk_cert_output;
  uint64_t mk_cert_generation = 0, mk_cert_cutover = pp::BeaconCertificate::kNoCutover;
  std::vector<std::string> mk_cert_hints;
  mk_cert_cmd->add_option("--network-id", mk_cert_network, "The chain's networkId")->required();
  mk_cert_cmd->add_option("--generation", mk_cert_generation, "The new generation (current + 1)")->required();
  mk_cert_cmd->add_option("--pubkey", mk_cert_pubkey, "New beacon signing public key (hex or .pub file)")
      ->required();
  mk_cert_cmd->add_option("--cutover", mk_cert_cutover,
                          "Last block the previous generation may sign — when its key is stolen. Reads the "
                          "block from the node given with -b --host, fills its hash and shows what the cut "
                          "drops. Default: none (an honest rotation)");
  mk_cert_cmd->add_option("--dial-hint", mk_cert_hints,
                          "A relay in front of the new beacon (ADP multiaddr), where nodes may look for it; "
                          "repeat for more (at most 8)");
  mk_cert_cmd->add_option("-o,--output", mk_cert_output, "Output file (must not exist)")->required();

  // mk-recovery (beacon-authority ba7)
  auto* mk_recovery_cmd = app.add_subcommand(
      "mk-recovery", "Create an unsigned recovery announcement (new admin set) for the recovery keys to sign");
  std::string mk_recovery_network, mk_recovery_output;
  std::vector<std::string> mk_recovery_admins;
  unsigned mk_recovery_min_sig = 0;
  mk_recovery_cmd->add_option("--network-id", mk_recovery_network, "The chain's networkId")->required();
  mk_recovery_cmd->add_option("--admin-pubkey", mk_recovery_admins,
                              "A new admin public key (hex or .pub file); repeat for each (at least 3)")
      ->required();
  mk_recovery_cmd->add_option("--min-signatures", mk_recovery_min_sig, "Signatures the new admin set requires")
      ->required();
  mk_recovery_cmd->add_option("-o,--output", mk_recovery_output, "Output file (must not exist)")->required();

  // mk-equivocation / add-equivocation (beacon-authority ba6)
  auto* mk_equiv_cmd = app.add_subcommand(
      "mk-equivocation", "Sign equivocation evidence with a beacon key (drills: it halts the nodes that take it)");
  std::string mk_equiv_network, mk_equiv_key, mk_equiv_output;
  uint64_t mk_equiv_generation = 0, mk_equiv_height = 0;
  mk_equiv_cmd->add_option("--network-id", mk_equiv_network, "The chain's networkId")->required();
  mk_equiv_cmd->add_option("--generation", mk_equiv_generation, "The beacon generation of the key")->required();
  mk_equiv_cmd->add_option("--height", mk_equiv_height, "The block height both tips claim")->required();
  mk_equiv_cmd->add_option("-k,--key", mk_equiv_key, "Beacon signing private key (hex or file)")->required();
  mk_equiv_cmd->add_option("-o,--output", mk_equiv_output, "Output file (must not exist)")->required();
  auto* add_equiv_cmd = app.add_subcommand(
      "add-equivocation", "Submit equivocation evidence from a file (to a relay or the beacon, -b; or a miner, -m)");
  std::string add_equiv_file;
  add_equiv_cmd->add_option("file", add_equiv_file, "Evidence file (mk-equivocation, or a node's equivocation.dat)")
      ->required();

  // mk-account: create unsigned T_NEW_USER transaction file
  auto* mk_account_cmd = app.add_subcommand("mk-account",
                                            "Create unsigned T_NEW_USER transaction file");
  uint64_t mk_acc_from = 0, mk_acc_to = 0;
  uint64_t mk_acc_amount = 0;
  uint64_t mk_acc_fee = 0;
  std::string mk_acc_pubkey;
  std::string mk_acc_meta;
  uint8_t mk_acc_min_sig = 1;
  std::string mk_acc_output;
  mk_account_cmd->add_option("from", mk_acc_from, "From wallet ID (funding account)")->required();
  mk_account_cmd->add_option("amount", mk_acc_amount, "Initial balance")->required();
  mk_account_cmd->add_option("-t,--to", mk_acc_to,
                             "New account ID; if omitted, a random ID is generated")
      ->default_val(0);
  mk_account_cmd->add_option("-f,--fee", mk_acc_fee, "Transaction fee (default: 0)")->default_val(0);
  mk_account_cmd->add_option("--new-pubkey", mk_acc_pubkey,
                             "New account public key (hex); if omitted, key pair is auto-generated");
  mk_account_cmd->add_option("-m,--meta", mk_acc_meta, "Account description")->default_val("");
  mk_account_cmd->add_option("--min-signatures", mk_acc_min_sig,
                             "Required signatures (default: 1)")
      ->default_val(1);
  mk_account_cmd->add_option("-o,--output", mk_acc_output, "Output file (must not exist)")
      ->required();

  // add-account: create, sign, and submit T_NEW_USER transaction
  auto* add_account_cmd =
      app.add_subcommand("add-account", "Create and submit T_NEW_USER account creation");
  uint64_t add_acc_from = 0, add_acc_to = 0;
  uint64_t add_acc_amount = 0;
  uint64_t add_acc_fee = 0;
  std::string add_acc_pubkey;
  std::string add_acc_meta;
  uint8_t add_acc_min_sig = 1;
  std::string add_account_key;
  add_account_cmd->add_option("from", add_acc_from, "From wallet ID (funding account)")
      ->required();
  add_account_cmd->add_option("amount", add_acc_amount, "Initial balance")->required();
  add_account_cmd->add_option("-t,--to", add_acc_to,
                               "New account ID; if omitted, a random ID is generated")
      ->default_val(0);
  add_account_cmd->add_option("-f,--fee", add_acc_fee, "Transaction fee (default: 0)")
      ->default_val(0);
  add_account_cmd->add_option("--new-pubkey", add_acc_pubkey,
                               "New account public key (hex); if omitted, key pair is auto-generated");
  add_account_cmd->add_option("-m,--meta", add_acc_meta, "Account description")->default_val("");
  add_account_cmd->add_option("--min-signatures", add_acc_min_sig,
                              "Required signatures (default: 1)")
      ->default_val(1);
  add_account_cmd
      ->add_option("-k,--key", add_account_key,
                   "Private key (hex or file) of funding account to sign")
      ->required();

  // load-setup / load: the load generator (projects/load-resilience)
  auto* load_setup_cmd = app.add_subcommand(
      "load-setup", "Create funded accounts for `load` (T_NEW_USER from a funding account); writes their keys");
  pp::load::SetupConfig loadSetup;
  uint64_t loadSetupTimeout = 300;
  std::string loadSetupOut;
  load_setup_cmd->add_option("--from", loadSetup.from, "Funding account id")->required();
  load_setup_cmd->add_option("-k,--key", loadSetup.fromKeys, "Funding account private key (hex or file; repeat for each signature)")
      ->required();
  load_setup_cmd->add_option("-n,--accounts", loadSetup.accounts, "Accounts to create")->default_val(100);
  load_setup_cmd->add_option("--amount", loadSetup.amount, "Balance of each")->default_val(100000);
  load_setup_cmd->add_option("-f,--fee", loadSetup.fee, "Fee of each creation")->default_val(1);
  load_setup_cmd->add_option("--timeout", loadSetupTimeout, "Seconds to wait for the accounts to exist")->default_val(300);
  load_setup_cmd->add_option("-o,--output", loadSetupOut, "Accounts file to write (must not exist)")->required();

  auto* load_cmd = app.add_subcommand(
      "load", "Send requests at a fixed rate whatever the replies, classify every reply, follow transfers into blocks");
  pp::load::RunConfig loadRun;
  std::string loadAccounts, loadMix = "tx_add", loadJson;
  uint64_t loadDuration = 10, loadBurstAt = 0, loadDrain = 60;
  load_cmd->add_option("--accounts", loadAccounts, "Accounts file from load-setup (needed for transfers)");
  load_cmd->add_option("--mix", loadMix,
                       "Kinds and weights: tx_add, tx_junk, status, account_get, block_get, tx_get_by_wallet, big, "
                       "block_wait, register "
                       "(e.g. tx_add=9,tx_junk=1)")
      ->default_val("tx_add");
  load_cmd->add_option("-r,--rate", loadRun.rate, "Requests per second, all kinds")->default_val(10);
  load_cmd->add_option("-d,--duration", loadDuration, "Seconds of steady load")->default_val(10);
  load_cmd->add_option("--burst", loadRun.burst, "Valid transfers sent at once, at --burst-at")->default_val(0);
  load_cmd->add_option("--burst-at", loadBurstAt, "Seconds into the run for the burst")->default_val(0);
  load_cmd->add_option("--peers", loadRun.peers, "Client identities to send from")->default_val(4);
  load_cmd->add_option("--amount", loadRun.amount, "Amount of each transfer")->default_val(1);
  load_cmd->add_option("-f,--fee", loadRun.fee, "Fee of each transfer")->default_val(1);
  load_cmd->add_option("--drain", loadDrain, "Seconds to follow blocks after the last send")->default_val(60);
  uint64_t loadQuiet = 15;
  load_cmd->add_option("--quiet", loadQuiet, "Stop following once no transfer sent was included for this many seconds")
      ->default_val(15);
  load_cmd->add_option("--json", loadJson, "Also write the report as JSON to this file");

  // sign-tx: add a signature to an existing tx file
  auto* sign_tx_cmd = app.add_subcommand("sign-tx", "Add signature to a transaction file");
  std::string sign_tx_file;
  std::string sign_key;
  std::string sign_network_id;
  sign_tx_cmd->add_option("file", sign_tx_file, "Transaction file to sign")->required();
  sign_tx_cmd->add_option("-k,--key", sign_key, "Private key (hex or file) to sign")
      ->required();
  sign_tx_cmd->add_option("--network-id", sign_network_id,
                          "Chain networkId for the signing message (must match genesis)");

  // submit-tx: submit signed tx file (relay / beacon)
  auto* submit_tx_cmd = app.add_subcommand("submit-tx", "Submit a signed transaction file (to a relay or the beacon, -b)");
  std::string submit_tx_file;
  submit_tx_cmd->add_option("file", submit_tx_file, "Signed transaction file")->required();

  CLI11_PARSE(app, argc, argv);

  // Handle keygen command (no server connection needed)
  if (export_state_cmd->parsed()) {
    pp::logging::getRootLogger().setLevel(debug ? pp::logging::Level::DEBUG : pp::logging::Level::WARNING);
    return runExportState(exportDir, exportHeight, exportOut, exportScratch);
  }

  if (peer_id_cmd->parsed()) {
    auto privateKey = pp::utl::readPrivateKey(peerIdKey, "");
    if (!privateKey) {
      std::cerr << "Error: " << privateKey.error().message << "\n";
      return 1;
    }
    auto amp = pp::network::LedgerAmpConfigFromPrivateKey(privateKey.value(), 0);
    auto publicKey = pp::network::PublicKeyFromPrivateKey(privateKey.value());
    if (!amp || !publicKey) {
      std::cerr << "Error: not an ML-DSA-65 private key\n";
      return 1;
    }
    std::cout << "Peer id:          " << amp.value().local_peer_id << "\n";
    std::cout << "Public key (hex): " << pp::utl::hexEncode(publicKey.value()) << "\n";
    return 0;
  }

  if (keygen->parsed()) {
    auto pair = pp::utl::mlDsaGenerate();
    if (!pair.isOk()) {
      std::cerr << "Error: " << pair.error().message << "\n";
      return 1;
    }
    if (!keygenOut.empty()) {
      auto keyWritten = pp::utl::writeToNewFile(keygenOut + ".key", pp::utl::hexEncode(pair->privateKey) + "\n");
      if (!keyWritten) {
        std::cerr << "Error: " << keygenOut << ".key: " << keyWritten.error().message << "\n";
        return 1;
      }
      auto pubWritten = pp::utl::writeToNewFile(keygenOut + ".pub", pp::utl::hexEncode(pair->publicKey) + "\n");
      if (!pubWritten) {
        std::cerr << "Error: " << keygenOut << ".pub: " << pubWritten.error().message << "\n";
        return 1;
      }
      std::cout << "Wrote " << keygenOut << ".key (keep secret) and " << keygenOut << ".pub\n";
      return 0;
    }
    std::cout << "ML-DSA-65 key pair generated.\n";
    std::cout << "Public key (hex):   " << pp::utl::hexEncode(pair->publicKey) << "\n";
    std::cout << "Private key (hex):  " << pp::utl::hexEncode(pair->privateKey) << "\n";
    std::cout << "\nKeep the private key secret. Use the public key in init-config.json (or -o <name> to write files).\n";
    return 0;
  }

  // Handle mk-tx (no server connection needed)
  if (mk_tx_cmd->parsed()) {
    return runMkTx(mk_from, mk_to, mk_amount, mk_output);
  }

  const bool mkCertNeedsNode = mk_cert_cmd->parsed() && mk_cert_cutover != pp::BeaconCertificate::kNoCutover;
  if (mk_cert_cmd->parsed() && !mkCertNeedsNode) {
    return runMkBeaconCert(mk_cert_network, mk_cert_generation, mk_cert_pubkey, mk_cert_cutover, "", mk_cert_hints,
                           mk_cert_output);
  }
  if (mkCertNeedsNode && !connectToBeacon) {
    std::cerr << "Error: --cutover reads the block from a node: give -b --host <relay or beacon multiaddr>.\n";
    return 1;
  }

  if (mk_recovery_cmd->parsed()) {
    return runMkRecovery(mk_recovery_network, mk_recovery_admins, mk_recovery_min_sig, mk_recovery_output);
  }

  if (mk_equiv_cmd->parsed()) {
    return runMkEquivocation(mk_equiv_network, mk_equiv_generation, mk_equiv_height, mk_equiv_key, mk_equiv_output);
  }

  // Handle mk-account (no server connection needed)
  if (mk_account_cmd->parsed()) {
    uint64_t mk_acc_to_resolved = mk_acc_to;
    if (mk_acc_to_resolved == 0) mk_acc_to_resolved = randomAccountId();
    return runMkAccount(mk_acc_from, mk_acc_to_resolved, mk_acc_amount, mk_acc_fee, mk_acc_pubkey,
                       mk_acc_meta, mk_acc_min_sig, mk_acc_output, mk_acc_to == 0);
  }

  // Handle sign-tx (no server connection needed)
  if (sign_tx_cmd->parsed()) {
    return runSignTx(sign_tx_file, sign_key, sign_network_id);
  }

  if (!hostFile.empty()) {
    auto content = readFileContent(hostFile);
    if (!content) {
      std::cerr << "Error: " << content.error().message << "\n";
      return 1;
    }
    host = *content;
    while (!host.empty() && std::isspace(static_cast<unsigned char>(host.back()))) {
      host.pop_back(); // the trailing newline
    }
  }

  // For server commands, validate beacon/miner flag (health speaks to any role)
  if (!connectToBeacon && !connectToMiner && !health_cmd->parsed() && !check_predecessor_cmd->parsed()) {
    std::cerr << "Error: Must specify -b/--beacon or -m/--miner for server commands.\n";
    std::cerr << "Run '" << argv[0] << " --help' for more information.\n";
    return 1;
  }

  if (connectToBeacon && connectToMiner) {
    std::cerr << "Error: Cannot connect to both beacon and miner.\n";
    return 1;
  }

  // Parse host:port format if present
  std::string parsedHost = host;
  uint16_t parsedPort = port;
  uint16_t extractedPort = 0;
  if (pp::utl::parseHostPort(host, parsedHost, extractedPort)) {
    if (port == 0) {
      parsedPort = extractedPort;
    }
  }

  // Set default port if not specified
  if (parsedPort == 0) {
    parsedPort = connectToBeacon ? pp::Client::DEFAULT_BEACON_PORT
                                 : pp::Client::DEFAULT_MINER_PORT;
  }

  pp::logging::getRootLogger().setLevel(debug ? pp::logging::Level::DEBUG
                                              : pp::logging::Level::WARNING);

  std::string multiaddr = host;
  if (!multiaddr.empty() && multiaddr.front() != '/') {
    std::cerr << "Error: server address must be an ADP multiaddr (/ip4|dns4/.../udp/.../adp/1.0.0/p2p/...).\n";
    return 1;
  }

  if (load_setup_cmd->parsed()) {
    return runLoadSetup(multiaddr, loadSetup, loadSetupTimeout, loadSetupOut);
  }
  if (load_cmd->parsed()) {
    loadRun.quiet = std::chrono::seconds(loadQuiet);
    return runLoad(multiaddr, loadRun, loadAccounts, loadMix, loadDuration, loadBurstAt, loadDrain, loadJson);
  }

  auto ephemeralKeys = pp::utl::mlDsaGenerate();
  if (!ephemeralKeys) {
    std::cerr << "Error: failed to generate ephemeral AMP identity: " << ephemeralKeys.error().message << "\n";
    return 1;
  }
  auto ampCfg = pp::network::LedgerAmpConfigFromPrivateKey(ephemeralKeys->privateKey, 0);
  if (!ampCfg) {
    std::cerr << "Error: failed to configure AMP client: " << ampCfg.error().message << "\n";
    return 1;
  }
  pp::network::LedgerAmpRuntime ampRuntime;
  if (auto started = ampRuntime.Start(*ampCfg); !started) {
    std::cerr << "Error: failed to start AMP runtime: " << started.error().message << "\n";
    return 1;
  }

  pp::Client client;
  client.attachAmpTransport(ampRuntime, "remote");
  if (auto dial = client.setAmpPeer("remote", multiaddr); !dial) {
    std::cerr << "Error: failed to dial server: " << dial.error().message << "\n";
    return 1;
  }

  int exitCode = 0;

  if (mkCertNeedsNode) {
    auto hash = describeCutover(client, mk_cert_cutover);
    if (!hash) {
      std::cerr << "Error: " << hash.error().message << "\n";
      return 1;
    }
    return runMkBeaconCert(mk_cert_network, mk_cert_generation, mk_cert_pubkey, mk_cert_cutover, hash.value(),
                           mk_cert_hints, mk_cert_output);
  }

  if (check_predecessor_cmd->parsed()) {
    return runCheckPredecessor(client, predecessorDir);
  }
  if (health_cmd->parsed()) {
    return runHealth(client, healthReady);
  }

  // Handle beacon status command
  if (beacon_status->parsed() && connectToBeacon) {
    auto result = client.fetchBeaconState();
    if (result) {
      printBeaconStatus(result.value());
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle miner status command
  else if (beacon_status->parsed() && connectToMiner) {
    auto result = client.fetchMinerStatus();
    if (result) {
      std::cout << pp::common::io::metaToJsonString(result.value().ltsToMeta(), 2) << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle block command
  else if (block_cmd->parsed()) {
    auto result = client.fetchBlock(blockId);
    if (result) {
      std::cout << pp::common::io::metaToJsonString(result.value().ltsToMeta(), 2)
                << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle account command
  else if (account_cmd->parsed()) {
    auto result = client.fetchUserAccount(accountId);
    if (result) {
      std::cout << pp::common::io::metaToJsonString(result.value().ltsToMeta(), 2)
                << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle transactions-by-wallet command (beacon/relay/miner)
  else if (txs_cmd->parsed()) {
    pp::Client::TxGetByWalletRequest req;
    req.walletId = txs_walletId;
    req.beforeBlockId = txs_beforeBlockId;
    auto result = client.fetchTransactionsByWallet(req);
    if (result) {
      std::cout << pp::common::io::metaToJsonString(result.value().ltsToMeta(), 2)
                << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle transaction-by-index command (beacon/relay/miner)
  else if (tx_cmd->parsed()) {
    pp::Client::TxGetByIndexRequest req;
    req.txIndex = tx_index;
    auto result = client.fetchTransactionByIndex(req);
    if (result) {
      std::cout << pp::common::io::metaToJsonString(result.value().ltsToMeta(), 2)
                << "\n";
    } else {
      std::cerr << "Error: " << result.error().message << "\n";
      exitCode = 1;
    }
  }
  // Handle add-tx command (relay / beacon: transactions go up the tree)
  else if (add_tx_cmd->parsed()) {
    if (!connectToBeacon) {
      std::cerr << "Error: add-tx submits to a relay or the beacon: use -b/--beacon.\n";
      exitCode = 1;
    } else {
      exitCode = runAddTx(client, fromWalletId, toWalletId, amount, fee, key, addTxIdempotentId);
    }
  }
  // Handle miner-register / miner-revoke (relay / beacon)
  else if (miner_register_cmd->parsed() || miner_revoke_cmd->parsed()) {
    if (!connectToBeacon) {
      std::cerr << "Error: miner-register / miner-revoke submit to a relay or the beacon: use -b/--beacon.\n";
      exitCode = 1;
    } else {
      exitCode = runMinerTx(client,
                            miner_register_cmd->parsed() ? pp::Ledger::T_MINER_REGISTER
                                                         : pp::Ledger::T_MINER_REVOKE,
                            minerWalletId, minerFee, minerKey);
    }
  }
  // Handle add-account command (relay / beacon)
  else if (add_account_cmd->parsed()) {
    if (!connectToBeacon) {
      std::cerr << "Error: add-account submits to a relay or the beacon: use -b/--beacon.\n";
      exitCode = 1;
    } else {
      uint64_t add_acc_to_resolved = add_acc_to;
      if (add_acc_to_resolved == 0) add_acc_to_resolved = randomAccountId();
      exitCode = runAddAccount(client, add_acc_from, add_acc_to_resolved, add_acc_amount,
                               add_acc_fee, add_acc_pubkey, add_acc_meta, add_acc_min_sig,
                               add_account_key, add_acc_to == 0);
    }
  }
  // Handle submit-tx command (relay / beacon)
  else if (submit_tx_cmd->parsed()) {
    if (!connectToBeacon) {
      std::cerr << "Error: submit-tx submits to a relay or the beacon: use -b/--beacon.\n";
      exitCode = 1;
    } else {
      exitCode = runSubmitTx(client, submit_tx_file);
    }
  }
  // Handle add-equivocation (any role: each keeps it and passes it up)
  else if (add_equiv_cmd->parsed()) {
    exitCode = runAddEquivocation(client, add_equiv_file);
  }

  return exitCode;
}
