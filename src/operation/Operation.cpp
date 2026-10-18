#include "Operation.h"

#include "../chain/Types.h"
#include "../client/AccountIds.h"
#include "common/io/Json.h"
#include "lib/common/BinaryPack.hpp"
#include "lib/common/Utilities.h"

#include <sodium.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <type_traits>
#include <variant>

namespace pp::operation {

using pp::common::Object;
using pp::common::Value;

namespace {

const std::vector<std::pair<Kind, std::string>> &kindNames() {
  static const std::vector<std::pair<Kind, std::string>> names = {
      {Kind::Transfer, "transfer"},       {Kind::Mint, "mint"},
      {Kind::NewAccount, "new-account"},  {Kind::AccountUpdate, "account-update"},
      {Kind::ConfigUpdate, "config-update"},
      {Kind::BeaconCertificate, "beacon-certificate"}, {Kind::Recovery, "recovery"},
      {Kind::Genesis, "genesis"},         {Kind::Other, "record"}};
  return names;
}

std::string toBase64(const std::string &bytes) {
  std::string out(sodium_base64_encoded_len(bytes.size(), sodium_base64_VARIANT_ORIGINAL), '\0');
  sodium_bin2base64(out.data(), out.size(), reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size(),
                    sodium_base64_VARIANT_ORIGINAL);
  out.resize(out.find('\0'));
  return out;
}

std::optional<std::string> fromBase64(const std::string &text) {
  std::string out(text.size(), '\0');
  size_t length = 0;
  if (sodium_base642bin(reinterpret_cast<unsigned char *>(out.data()), out.size(), text.data(), text.size(), nullptr,
                        &length, nullptr, sodium_base64_VARIANT_ORIGINAL) != 0) {
    return std::nullopt;
  }
  out.resize(length);
  return out;
}

uint64_t randomNonZero() {
  uint64_t value = 0;
  while (value == 0) {
    randombytes_buf(&value, sizeof(value));
  }
  return value;
}

/** Each of `signatures` matched to the first of `keys` it verifies under (of `message`) not yet matched. */
std::vector<std::optional<size_t>> matchSignatures(const std::vector<std::string> &keys, const std::string &message,
                                                   const std::vector<std::string> &signatures) {
  std::vector<bool> used(keys.size(), false);
  std::vector<std::optional<size_t>> matched;
  for (const auto &signature : signatures) {
    std::optional<size_t> key;
    for (size_t i = 0; i < keys.size(); ++i) {
      if (!used[i] && utl::mlDsaVerify(keys[i], message, signature)) {
        key = i;
        used[i] = true;
        break;
      }
    }
    matched.push_back(key);
  }
  return matched;
}

/** The valid ones of `signatures`, one per key, in key order. */
std::vector<std::string> validSignatures(const std::vector<std::string> &keys, const std::string &message,
                                         const std::vector<std::string> &signatures) {
  const auto matched = matchSignatures(keys, message, signatures);
  std::vector<std::pair<size_t, std::string>> byKey;
  for (size_t i = 0; i < matched.size(); ++i) {
    if (matched[i]) {
      byKey.emplace_back(*matched[i], signatures[i]);
    }
  }
  std::sort(byKey.begin(), byKey.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
  std::vector<std::string> out;
  for (auto &entry : byKey) {
    out.push_back(std::move(entry.second));
  }
  return out;
}

/** A record of an operation and the signatures collected for it. */
template <typename Signatures> struct Part {
  const Ledger::Record *record;
  Signatures *signatures;
};

/** `record` with `signatures`, then each of `moreRecords` with its own (const as `op` is). */
template <typename Op> auto partsOf(Op &op) {
  using Signatures = std::conditional_t<std::is_const_v<Op>, const std::vector<std::string>, std::vector<std::string>>;
  std::vector<Part<Signatures>> parts{{&op.record, &op.signatures}};
  for (auto &more : op.moreRecords) {
    parts.push_back({&more, &more.signatures});
  }
  return parts;
}

std::string accountName(uint64_t id) {
  switch (id) {
  case AccountIds::ID_GENESIS:
    return "Genesis (admin)";
  case AccountIds::ID_RESERVE:
    return "Reserve";
  case AccountIds::ID_REGISTRAR:
    return "Registrar";
  case AccountIds::ID_FEE:
    return "Fee";
  case AccountIds::ID_RECYCLE:
    return "Recycle";
  default:
    return "Account " + std::to_string(id);
  }
}

std::string keysLine(const std::vector<std::string> &keys, uint32_t minSignatures) {
  std::ostringstream out;
  out << minSignatures << " of " << keys.size() << ":";
  for (size_t i = 0; i < keys.size(); ++i) {
    out << (i == 0 ? " " : ", ") << fingerprint(keys[i]);
  }
  return out.str();
}

template <typename Tx> void windowOf(const Tx &tx, Summary &summary) {
  if constexpr (std::is_base_of_v<Ledger::TxIdempotencyWindow, Tx>) {
    summary.validFrom = tx.validationTsMin;
    summary.validUntil = tx.validationTsMax;
  }
}

void describeConfig(const BlockChainConfig &c, Summary &s) {
  auto line = [&s](const std::string &label, uint64_t value) { s.lines.emplace_back(label, groupDigits(value)); };
  line("Slot (seconds)", c.slotDuration);
  line("Slots per epoch", c.slotsPerEpoch);
  line("Transactions per block", c.maxTransactionsPerBlock);
  line("Block bytes", c.maxBlockBytes);
  line("Transaction validity, longest (seconds)", c.maxValidationTimespanSeconds);
  line("Mint per epoch, most", c.maxIssuancePerEpoch);
  line("New account fee", c.newAccountFee);
  std::ostringstream fees;
  for (size_t i = 0; i < c.minFeeCoefficients.size(); ++i) {
    fees << (i ? ", " : "") << c.minFeeCoefficients[i];
  }
  s.lines.emplace_back("Fee coefficients", fees.str());
  line("Free meta bytes", c.freeCustomMetaSize);
  line("Meta bytes, most", c.maxCustomMetaSize);
  line("Heartbeat slots", c.heartbeatSlots);
  line("Miner stake, least", c.minerMinStake);
  line("Miner registration fee", c.minerRegistrationFee);
  line("Miner due after (slots)", c.minerDueAfterSlots);
  line("Miner missed slots, most", c.minerMaxMissedSlots);
  line("Checkpoint, least blocks", c.checkpoint.minBlocks);
  line("Checkpoint, least age (seconds)", c.checkpoint.minAgeSeconds);
}

} // namespace

std::string kindName(Kind kind) {
  for (const auto &[k, name] : kindNames()) {
    if (k == kind) {
      return name;
    }
  }
  return "record";
}

std::optional<Kind> kindFromName(const std::string &name) {
  for (const auto &[k, n] : kindNames()) {
    if (n == name) {
      return k;
    }
  }
  return std::nullopt;
}

Kind kindOf(const Ledger::Record &record) {
  switch (record.type) {
  case Ledger::T_DEFAULT: {
    auto tx = utl::binaryUnpack<Ledger::TxDefault>(record.data);
    if (tx && tx.value().fromWalletId == AccountIds::ID_GENESIS && tx.value().toWalletId == AccountIds::ID_RESERVE) {
      return Kind::Mint;
    }
    return Kind::Transfer;
  }
  case Ledger::T_NEW_USER:
    return Kind::NewAccount;
  case Ledger::T_USER_UPDATE:
    return Kind::AccountUpdate;
  case Ledger::T_CONFIG:
    return Kind::ConfigUpdate;
  case Ledger::T_BEACON_CERT:
    return Kind::BeaconCertificate;
  case Ledger::T_RECOVERY:
    return Kind::Recovery;
  case Ledger::T_GENESIS:
    return Kind::Genesis;
  default:
    return Kind::Other;
  }
}

std::optional<uint64_t> signerAccountOf(const Ledger::Record &record) {
  switch (record.type) {
  case Ledger::T_DEFAULT: {
    auto tx = utl::binaryUnpack<Ledger::TxDefault>(record.data);
    return tx ? std::optional<uint64_t>(tx.value().fromWalletId) : std::nullopt;
  }
  case Ledger::T_NEW_USER: {
    auto tx = utl::binaryUnpack<Ledger::TxNewUser>(record.data);
    return tx ? std::optional<uint64_t>(tx.value().fromWalletId) : std::nullopt;
  }
  case Ledger::T_USER_UPDATE: {
    auto tx = utl::binaryUnpack<Ledger::TxUserUpdate>(record.data);
    return tx ? std::optional<uint64_t>(tx.value().walletId) : std::nullopt;
  }
  case Ledger::T_CONFIG:
  case Ledger::T_BEACON_CERT:
  case Ledger::T_GENESIS:
    return AccountIds::ID_GENESIS;
  default:
    return std::nullopt;
  }
}

std::string signingMessage(const Operation &op) { return op.record.signingMessage(op.networkId); }

static std::vector<Value> base64All(const std::vector<std::string> &items) {
  std::vector<Value> out;
  for (const auto &item : items) {
    out.emplace_back(toBase64(item));
  }
  return out;
}

static pp::Roe<std::vector<std::string>> fromBase64All(const pp::common::Array *array, const std::string &what) {
  std::vector<std::string> out;
  if (!array) {
    return out;
  }
  for (const auto &value : array->elements) {
    auto bytes = fromBase64(pp::common::asString(value).value_or(""));
    if (!bytes) {
      return pp::Error("Operation file has a malformed " + what);
    }
    out.push_back(std::move(*bytes));
  }
  return out;
}

std::string toJson(const Operation &op) {
  Object record;
  record.setJsonUInt("type", op.record.type);
  record.set("data", toBase64(op.record.data));
  Object signers;
  if (op.signers.account) {
    signers.setJsonUInt("account", *op.signers.account);
  }
  signers.setJsonUInt("minSignatures", op.signers.minSignatures);
  std::vector<Value> keys;
  for (const auto &key : op.signers.publicKeys) {
    keys.emplace_back(toBase64(key));
  }
  signers.set("publicKeys", Object::array(std::move(keys)));
  std::vector<Value> signatures;
  for (const auto &signature : op.signatures) {
    signatures.emplace_back(toBase64(signature));
  }
  Object root;
  root.set("format", kFormat);
  root.setJsonUInt("version", kVersion);
  root.set("networkId", op.networkId);
  root.set("genesisHash", utl::hexEncode(op.genesisHash));
  root.set("kind", kindName(kindOf(op.record)));
  root.set("record", record);
  root.set("signers", signers);
  root.set("signatures", Object::array(std::move(signatures)));
  if (!op.moreRecords.empty()) {
    std::vector<Value> more;
    for (const auto &extra : op.moreRecords) {
      auto entry = std::make_shared<Object>();
      entry->setJsonUInt("type", extra.type);
      entry->set("data", toBase64(extra.data));
      entry->set("signatures", Object::array(base64All(extra.signatures)));
      more.emplace_back(std::move(entry));
    }
    root.set("moreRecords", Object::array(std::move(more)));
  }
  root.set("notes", op.notes);
  root.set("createdAt", static_cast<int64_t>(op.createdAt));
  return pp::common::io::objectToJsonString(root, 2) + "\n";
}

/** The signers' account must be the one the chain checks the signatures against. */
static pp::Roe<void> checkSignerAccount(const Ledger::Record &record, const std::optional<uint64_t> &account) {
  const Kind kind = kindOf(record);
  if (kind == Kind::Other) {
    return {};
  }
  const auto expected = signerAccountOf(record);
  if (account == expected) {
    return {};
  }
  return pp::Error("a " + kindName(kind) + " is signed by " +
                   (expected ? "account " + std::to_string(*expected) : std::string("a key set, not an account")) +
                   ", the signers name " +
                   (account ? "account " + std::to_string(*account) : std::string("no account")));
}

pp::Roe<Operation> fromJson(const std::string &json) {
  Object root;
  if (!pp::common::io::objectFromJsonString(root, json)) {
    return pp::Error("Not an operation file: not JSON");
  }
  if (root.getString("format").value_or("") != kFormat) {
    return pp::Error("Not an operation file: format is not " + std::string(kFormat));
  }
  const auto version = root.getNonNegInt("version");
  if (!version || *version != kVersion) {
    return pp::Error("Operation file version " + (version ? std::to_string(*version) : std::string("?")) +
                     " is not supported (this tool reads version " + std::to_string(kVersion) + ")");
  }
  Operation op;
  op.networkId = root.getString("networkId").value_or("");
  if (op.networkId.empty()) {
    return pp::Error("Operation file names no network");
  }
  if (const auto hash = root.getString("genesisHash"); hash && !hash->empty()) {
    op.genesisHash = utl::hexDecode(*hash);
    if (op.genesisHash.size() != utl::SHA256_DIGEST_SIZE) {
      return pp::Error("Operation file genesisHash is not a 32-byte hex hash");
    }
  }
  const Object *record = root.getObject("record");
  if (!record) {
    return pp::Error("Operation file has no record");
  }
  const auto type = record->getNonNegInt("type");
  const auto data = fromBase64(record->getString("data").value_or(""));
  if (!type || *type > UINT16_MAX || !data) {
    return pp::Error("Operation file record is malformed");
  }
  op.record.type = static_cast<uint16_t>(*type);
  op.record.data = *data;
  if (!op.record.decode()) {
    return pp::Error("Operation file record does not decode as a type " + std::to_string(*type) + " transaction");
  }
  op.kind = kindOf(op.record);
  if (const auto claimed = root.getString("kind"); !claimed || kindFromName(*claimed) != op.kind) {
    return pp::Error("Operation file says it is a " + claimed.value_or("?") + ", its record is a " +
                     kindName(op.kind));
  }
  const Object *signers = root.getObject("signers");
  if (!signers) {
    return pp::Error("Operation file names no signers");
  }
  if (signers->contains("account")) {
    op.signers.account = signers->getNonNegInt("account");
    if (!op.signers.account) {
      return pp::Error("Operation file signers.account is not an account id");
    }
  }
  const auto minSignatures = signers->getNonNegInt("minSignatures");
  const auto *keys = signers->getArray("publicKeys");
  if (!minSignatures || *minSignatures == 0 || !keys) {
    return pp::Error("Operation file signers are malformed");
  }
  op.signers.minSignatures = static_cast<uint32_t>(*minSignatures);
  for (const auto &value : keys->elements) {
    auto key = fromBase64(pp::common::asString(value).value_or(""));
    if (!key || key->size() != utl::kMlDsaPublicKeyBytes) {
      return pp::Error("Operation file has a signer key that is not an ML-DSA-65 public key");
    }
    op.signers.publicKeys.push_back(*key);
  }
  if (op.signers.minSignatures > op.signers.publicKeys.size()) {
    return pp::Error("Operation file needs more signatures than it has signers");
  }
  if (auto ok = checkSignerAccount(op.record, op.signers.account); !ok) {
    return pp::Error("Operation file: " + ok.error().message);
  }
  if (const auto *signatures = root.getArray("signatures")) {
    for (const auto &value : signatures->elements) {
      auto signature = fromBase64(pp::common::asString(value).value_or(""));
      if (!signature) {
        return pp::Error("Operation file has a malformed signature");
      }
      op.signatures.push_back(*signature);
    }
  }
  if (const auto *more = root.getArray("moreRecords")) {
    if (op.kind != Kind::Genesis) {
      return pp::Error("Operation file: only a genesis carries more records");
    }
    for (const auto &value : more->elements) {
      const Object *entry = pp::common::asObject(value);
      const auto moreType = entry ? entry->getNonNegInt("type") : std::nullopt;
      const auto moreData = entry ? fromBase64(entry->getString("data").value_or("")) : std::nullopt;
      if (!moreType || *moreType > UINT16_MAX || !moreData) {
        return pp::Error("Operation file has a malformed record in moreRecords");
      }
      Ledger::Record extra;
      extra.type = static_cast<uint16_t>(*moreType);
      extra.data = *moreData;
      // Block 0 creates the system accounts and miners, each signed by the genesis account.
      if (extra.type != Ledger::T_NEW_USER || !extra.decode() || signerAccountOf(extra) != AccountIds::ID_GENESIS) {
        return pp::Error("Operation file: a genesis's more records are accounts the genesis account creates");
      }
      auto signatures = fromBase64All(entry->getArray("signatures"), "signature");
      if (!signatures) {
        return signatures.error();
      }
      extra.signatures = std::move(signatures.value());
      op.moreRecords.push_back(std::move(extra));
    }
  }
  op.notes = root.getString("notes").value_or("");
  op.createdAt = root.getOrDefault("createdAt", int64_t{0});
  return op;
}

pp::Roe<Operation> load(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return pp::Error("Cannot read " + path);
  }
  std::ostringstream text;
  text << in.rdbuf();
  auto op = fromJson(text.str());
  if (!op) {
    return pp::Error(path + ": " + op.error().message);
  }
  return op;
}

pp::Roe<void> save(const Operation &op, const std::string &path, bool overwrite) {
  const std::string json = toJson(op);
  if (!overwrite) {
    return utl::writeToNewFile(path, json);
  }
  const std::string temp = path + ".tmp";
  std::error_code ec;
  std::filesystem::remove(temp, ec);
  if (auto written = utl::writeToNewFile(temp, json); !written) {
    return written;
  }
  std::filesystem::rename(temp, path, ec);
  if (ec) {
    return pp::Error("Cannot replace " + path + ": " + ec.message());
  }
  return {};
}

std::string fingerprint(const std::string &publicKey) {
  const std::string hex = utl::hexEncode(utl::sha256Raw(publicKey)).substr(0, 16);
  return hex.substr(0, 4) + "-" + hex.substr(4, 4) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4);
}

SignatureCheck check(const Operation &op) {
  SignatureCheck result;
  result.needed = op.signers.minSignatures;
  const auto &keys = op.signers.publicKeys;
  std::vector<bool> signedAll(keys.size(), true);
  for (const auto &part : partsOf(op)) {
    const auto matched = matchSignatures(keys, part.record->signingMessage(op.networkId), *part.signatures);
    std::vector<bool> signedThis(keys.size(), false);
    for (const auto &key : matched) {
      if (key) {
        signedThis[*key] = true;
      } else {
        ++result.invalid;
      }
    }
    for (size_t i = 0; i < keys.size(); ++i) {
      signedAll[i] = signedAll[i] && signedThis[i];
    }
  }
  for (size_t i = 0; i < keys.size(); ++i) {
    if (signedAll[i]) {
      result.signedBy.push_back(keys[i]);
    }
  }
  return result;
}

pp::Roe<std::string> sign(Operation &op, const std::string &privateKey) {
  const std::string message = signingMessage(op);
  auto signature = utl::mlDsaSign(privateKey, message);
  if (!signature) {
    return pp::Error("Cannot sign: " + signature.error().message);
  }
  std::optional<size_t> key;
  for (size_t i = 0; i < op.signers.publicKeys.size() && !key; ++i) {
    if (utl::mlDsaVerify(op.signers.publicKeys[i], message, signature.value())) {
      key = i;
    }
  }
  if (!key) {
    return pp::Error("This key is not one of the operation's signers");
  }
  const std::string &publicKey = op.signers.publicKeys[*key];
  const auto already = check(op);
  if (std::find(already.signedBy.begin(), already.signedBy.end(), publicKey) != already.signedBy.end()) {
    return pp::Error("This key has signed already (" + fingerprint(publicKey) + ")");
  }
  // Every record this key has not signed yet.
  for (const auto &part : partsOf(op)) {
    const std::string partMessage = part.record->signingMessage(op.networkId);
    const auto matched = matchSignatures(op.signers.publicKeys, partMessage, *part.signatures);
    if (std::find(matched.begin(), matched.end(), std::optional<size_t>(*key)) != matched.end()) {
      continue;
    }
    auto partSignature = part.record == &op.record ? signature : utl::mlDsaSign(privateKey, partMessage);
    if (!partSignature) {
      return pp::Error("Cannot sign: " + partSignature.error().message);
    }
    part.signatures->push_back(partSignature.value());
  }
  return publicKey;
}

pp::Roe<Operation> merge(const std::vector<Operation> &copies) {
  if (copies.empty()) {
    return pp::Error("Nothing to merge");
  }
  auto sameRecords = [](const Operation &a, const Operation &b) {
    if (a.record.type != b.record.type || a.record.data != b.record.data ||
        a.moreRecords.size() != b.moreRecords.size()) {
      return false;
    }
    for (size_t i = 0; i < a.moreRecords.size(); ++i) {
      if (a.moreRecords[i].type != b.moreRecords[i].type || a.moreRecords[i].data != b.moreRecords[i].data) {
        return false;
      }
    }
    return true;
  };
  Operation merged = copies.front();
  for (size_t i = 1; i < copies.size(); ++i) {
    const Operation &other = copies[i];
    if (other.networkId != merged.networkId || other.genesisHash != merged.genesisHash ||
        !sameRecords(other, merged) || !(other.signers == merged.signers) || other.notes != merged.notes ||
        other.createdAt != merged.createdAt) {
      return pp::Error("Copy " + std::to_string(i + 1) + " is a different operation: only signatures may differ");
    }
    merged.signatures.insert(merged.signatures.end(), other.signatures.begin(), other.signatures.end());
    for (size_t r = 0; r < merged.moreRecords.size(); ++r) {
      auto &into = merged.moreRecords[r].signatures;
      const auto &from = other.moreRecords[r].signatures;
      into.insert(into.end(), from.begin(), from.end());
    }
  }
  for (const auto &part : partsOf(merged)) {
    *part.signatures =
        validSignatures(merged.signers.publicKeys, part.record->signingMessage(merged.networkId), *part.signatures);
  }
  return merged;
}

pp::Roe<std::vector<Ledger::Record>> signedRecords(const Operation &op) {
  const auto checked = check(op);
  if (!checked.complete()) {
    return pp::Error("Signed by " + std::to_string(checked.signedBy.size()) + " of the " +
                     std::to_string(checked.needed) + " needed");
  }
  std::vector<Ledger::Record> out;
  for (const auto &part : partsOf(op)) {
    Ledger::Record record = *part.record;
    record.signatures =
        validSignatures(op.signers.publicKeys, part.record->signingMessage(op.networkId), *part.signatures);
    out.push_back(std::move(record));
  }
  return out;
}

pp::Roe<Ledger::Record> signedRecord(const Operation &op) {
  auto records = signedRecords(op);
  if (!records) {
    return records.error();
  }
  return records.value().front();
}

std::string groupDigits(uint64_t value) {
  std::string digits = std::to_string(value);
  for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3) {
    digits.insert(static_cast<size_t>(i), ",");
  }
  return digits;
}

pp::Roe<Summary> summarize(const Operation &op) {
  auto typed = op.record.decode();
  if (!typed) {
    return pp::Error("The record does not decode: " + typed.error().message);
  }
  Summary s;
  const Kind kind = kindOf(op.record);
  std::visit([&s](const auto &tx) { windowOf(tx, s); }, typed.value());
  switch (kind) {
  case Kind::Transfer: {
    const auto &tx = std::get<Ledger::TxDefault>(typed.value());
    s.title = "Transfer";
    s.lines = {{"From", accountName(tx.fromWalletId)}, {"To", accountName(tx.toWalletId)},
               {"Amount", groupDigits(tx.amount) + (tx.tokenId ? " of token " + std::to_string(tx.tokenId) : "")},
               {"Fee", groupDigits(tx.fee)}};
    break;
  }
  case Kind::Mint: {
    const auto &tx = std::get<Ledger::TxDefault>(typed.value());
    s.title = "Mint new tokens to the reserve";
    s.lines = {{"Amount", groupDigits(tx.amount)}, {"Fee", groupDigits(tx.fee)}};
    break;
  }
  case Kind::NewAccount: {
    const auto &tx = std::get<Ledger::TxNewUser>(typed.value());
    s.title = "Create an account";
    s.lines = {{"Account", accountName(tx.toWalletId)}, {"Funded by", accountName(tx.fromWalletId)},
               {"Amount", groupDigits(tx.amount)}, {"Fee", groupDigits(tx.fee)}};
    UserAccount account;
    if (account.ltsFromString(tx.meta)) {
      s.lines.emplace_back("Its keys", keysLine(account.wallet.publicKeys, account.wallet.minSignatures));
    } else {
      s.lines.emplace_back("Its keys", "(the account record does not decode)");
    }
    break;
  }
  case Kind::AccountUpdate: {
    const auto &tx = std::get<Ledger::TxUserUpdate>(typed.value());
    s.title = "Replace an account's keys";
    UserAccount account;
    if (!account.ltsFromString(tx.meta)) {
      return pp::Error("The account update does not decode");
    }
    s.lines = {{"Account", accountName(tx.walletId)},
               {"Its keys from then on", keysLine(account.wallet.publicKeys, account.wallet.minSignatures)},
               {"Fee", groupDigits(tx.fee)}};
    break;
  }
  case Kind::ConfigUpdate: {
    const auto &tx = std::get<Ledger::TxConfig>(typed.value());
    s.title = "Change the chain's config (from the next epoch)";
    GenesisAccountMeta meta;
    if (!meta.ltsFromString(tx.meta)) {
      return pp::Error("The config update does not decode");
    }
    describeConfig(meta.config, s);
    s.lines.emplace_back("Admin keys", keysLine(meta.genesis.wallet.publicKeys, meta.genesis.wallet.minSignatures));
    break;
  }
  case Kind::BeaconCertificate: {
    const auto &tx = std::get<Ledger::TxBeaconCert>(typed.value());
    s.title = "Certify a new beacon key";
    BeaconCertificate cert;
    if (!cert.ltsFromString(tx.meta)) {
      return pp::Error("The beacon certificate does not decode");
    }
    s.lines = {{"Generation", std::to_string(cert.generation)},
               {"Beacon key", fingerprint(cert.publicKey)},
               {"Cutover", cert.hasCutover() ? "block " + groupDigits(cert.cutoverHeight) + " (blocks after it are dropped)"
                                             : "none"},
               {"Dial hints", std::to_string(cert.dialHints.size())}};
    break;
  }
  case Kind::Recovery: {
    const auto &tx = std::get<Ledger::TxRecovery>(typed.value());
    s.title = "Replace the admin keys (recovery)";
    RecoveryAnnouncement announcement;
    if (!announcement.ltsFromString(tx.meta)) {
      return pp::Error("The recovery announcement does not decode");
    }
    s.lines = {{"New admin keys", keysLine(announcement.adminPublicKeys, announcement.adminMinSignatures)}};
    break;
  }
  case Kind::Genesis: {
    const auto &tx = std::get<Ledger::TxGenesis>(typed.value());
    s.title = "Start the chain (genesis)";
    GenesisAccountMeta meta;
    if (!meta.ltsFromString(tx.meta)) {
      return pp::Error("The genesis record does not decode");
    }
    s.lines.emplace_back("Starts", utl::formatTimestampLocal(meta.config.genesisTime));
    describeConfig(meta.config, s);
    s.lines.emplace_back("Admin keys", keysLine(meta.genesis.wallet.publicKeys, meta.genesis.wallet.minSignatures));
    const auto &recovery = meta.authority.recovery;
    s.lines.emplace_back("Recovery keys", keysLine(recovery.publicKeys, recovery.minSignatures) + "; takes effect " +
                                              std::to_string(recovery.delayEpochs) + " epoch(s) after announcing");
    s.lines.emplace_back("Beacon key (generation " + std::to_string(meta.authority.beacon.generation) + ")",
                         fingerprint(meta.authority.beacon.publicKey));
    for (const auto &extra : op.moreRecords) {
      auto account = utl::binaryUnpack<Ledger::TxNewUser>(extra.data);
      UserAccount created;
      if (!account || !created.ltsFromString(account.value().meta)) {
        return pp::Error("A genesis account record does not decode");
      }
      const uint64_t id = account.value().toWalletId;
      const bool miner = created.wallet.miner.registered;
      std::string line = keysLine(created.wallet.publicKeys, created.wallet.minSignatures);
      if (account.value().amount > 0) {
        line = groupDigits(account.value().amount) + (miner ? " staked; keys " : "; keys ") + line;
      }
      s.lines.emplace_back(miner ? "Miner " + std::to_string(id) : accountName(id), line);
    }
    break;
  }
  case Kind::Other:
    s.title = "Transaction of type " + std::to_string(op.record.type);
    break;
  }
  s.lines.insert(s.lines.begin(), {"Network", op.networkId});
  return s;
}

Window windowFrom(int64_t now, int64_t validForSeconds) {
  Window window;
  window.from = now - 60;
  window.until = window.from + validForSeconds;
  return window;
}

namespace {

Operation start(const Common &common) {
  Operation op;
  op.networkId = common.networkId;
  op.genesisHash = common.genesisHash;
  op.signers = common.signers;
  op.notes = common.notes;
  op.createdAt = common.createdAt;
  return op;
}

template <typename Tx> void stamp(Tx &tx, const Common &common) {
  tx.idempotentId = common.idempotentId ? common.idempotentId : randomNonZero();
  tx.validationTsMin = common.window.from;
  tx.validationTsMax = common.window.until;
}

pp::Roe<void> checkCommon(const Common &common, bool needsAccount) {
  if (common.networkId.empty()) {
    return pp::Error("No network id");
  }
  if (common.signers.publicKeys.empty() || common.signers.minSignatures == 0 ||
      common.signers.minSignatures > common.signers.publicKeys.size()) {
    return pp::Error("The signers need at least one key, and no more signatures than keys");
  }
  if (needsAccount && !common.signers.account) {
    return pp::Error("The signers' account is not given");
  }
  if (common.window.until <= common.window.from) {
    return pp::Error("The validity window is empty");
  }
  return {};
}

} // namespace

pp::Roe<Operation> transfer(const Common &common, uint64_t to, uint64_t amount, uint64_t fee, uint64_t tokenId) {
  if (auto ok = checkCommon(common, true); !ok) {
    return ok.error();
  }
  Ledger::TxDefault tx;
  tx.tokenId = tokenId;
  tx.fromWalletId = *common.signers.account;
  tx.toWalletId = to;
  tx.amount = amount;
  tx.fee = fee;
  stamp(tx, common);
  Operation op = start(common);
  op.record.type = Ledger::T_DEFAULT;
  op.record.data = utl::binaryPack(tx);
  op.kind = kindOf(op.record);
  return op;
}

pp::Roe<Operation> mint(const Common &common, uint64_t amount, uint64_t fee) {
  Common genesis = common;
  genesis.signers.account = AccountIds::ID_GENESIS;
  return transfer(genesis, AccountIds::ID_RESERVE, amount, fee);
}

pp::Roe<Operation> newAccount(const Common &common, uint64_t id, const UserAccount &account, uint64_t amount,
                              uint64_t fee) {
  if (auto ok = checkCommon(common, true); !ok) {
    return ok.error();
  }
  Ledger::TxNewUser tx;
  tx.fromWalletId = *common.signers.account;
  tx.toWalletId = id;
  tx.amount = amount;
  tx.fee = fee;
  tx.meta = account.ltsToString();
  stamp(tx, common);
  Operation op = start(common);
  op.record.type = Ledger::T_NEW_USER;
  op.record.data = utl::binaryPack(tx);
  op.kind = Kind::NewAccount;
  return op;
}

pp::Roe<Operation> genesis(const std::vector<Ledger::Record> &records, const std::string &notes, int64_t createdAt) {
  if (records.empty() || records.front().type != Ledger::T_GENESIS) {
    return pp::Error("A genesis starts with the genesis record");
  }
  auto tx = utl::binaryUnpack<Ledger::TxGenesis>(records.front().data);
  GenesisAccountMeta meta;
  if (!tx || !meta.ltsFromString(tx.value().meta)) {
    return pp::Error("The genesis record does not decode");
  }
  Operation op;
  op.networkId = meta.config.networkId;
  op.signers.account = AccountIds::ID_GENESIS;
  op.signers.publicKeys = meta.genesis.wallet.publicKeys;
  op.signers.minSignatures = meta.genesis.wallet.minSignatures;
  op.record.type = records.front().type;
  op.record.data = records.front().data;
  op.kind = Kind::Genesis;
  for (size_t i = 1; i < records.size(); ++i) {
    const auto &extra = records[i];
    if (extra.type != Ledger::T_NEW_USER || !extra.decode() || signerAccountOf(extra) != AccountIds::ID_GENESIS) {
      return pp::Error("Record " + std::to_string(i) + " of the genesis is not an account the genesis account creates");
    }
    Ledger::Record unsigned_;
    unsigned_.type = extra.type;
    unsigned_.data = extra.data;
    op.moreRecords.push_back(std::move(unsigned_));
  }
  op.notes = notes;
  op.createdAt = createdAt;
  if (op.networkId.empty() || op.signers.publicKeys.empty() || op.signers.minSignatures == 0 ||
      op.signers.minSignatures > op.signers.publicKeys.size()) {
    return pp::Error("The genesis record names no network, or no admin keys to sign it");
  }
  return op;
}

pp::Roe<Operation> replaceKeys(const Common &common, const UserAccount &current, const std::vector<std::string> &keys,
                               uint32_t minSignatures, uint64_t fee) {
  if (auto ok = checkCommon(common, true); !ok) {
    return ok.error();
  }
  if (keys.empty() || minSignatures == 0 || minSignatures > keys.size() || minSignatures > 255) {
    return pp::Error("The new keys: at least one, and between 1 and that many to sign");
  }
  for (const auto &key : keys) {
    if (key.size() != utl::kMlDsaPublicKeyBytes || std::count(keys.begin(), keys.end(), key) > 1) {
      return pp::Error("The new keys must be distinct ML-DSA-65 public keys");
    }
  }
  UserAccount next = current;
  next.wallet.publicKeys = keys;
  next.wallet.minSignatures = static_cast<uint8_t>(minSignatures);
  // The update carries the balances as they will be: the fee comes out of the native token.
  int64_t &native = next.wallet.mBalances[AccountIds::ID_GENESIS];
  if (fee > static_cast<uint64_t>(native)) {
    return pp::Error("The account cannot pay the fee (" + groupDigits(fee) + ")");
  }
  native -= static_cast<int64_t>(fee);
  Ledger::TxUserUpdate tx;
  tx.walletId = *common.signers.account;
  tx.fee = fee;
  tx.meta = next.ltsToString();
  stamp(tx, common);
  Operation op = start(common);
  op.record.type = Ledger::T_USER_UPDATE;
  op.record.data = utl::binaryPack(tx);
  op.kind = Kind::AccountUpdate;
  return op;
}

pp::Roe<Operation> wrap(const Common &common, const Ledger::Record &record) {
  if (auto ok = checkCommon(common, false); !ok) {
    return ok.error();
  }
  if (!record.decode()) {
    return pp::Error("The record does not decode as a type " + std::to_string(record.type) + " transaction");
  }
  if (auto ok = checkSignerAccount(record, common.signers.account); !ok) {
    return pp::Error("Wrong signers: " + ok.error().message);
  }
  Operation op = start(common);
  op.record.type = record.type;
  op.record.data = record.data;
  op.kind = kindOf(op.record);
  return op;
}

} // namespace pp::operation
