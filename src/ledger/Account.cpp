#include "Account.h"

#include "common/Serialize.hpp"
#include "lib/common/Utilities.h"

#include <sstream>

namespace pp {

std::ostream& operator<<(std::ostream& os, const Wallet& wallet) {
  os << "Wallet{balances: {";
  bool first = true;
  for (const auto& [tokenId, balance] : wallet.mBalances) {
    if (!first) os << ", ";
    os << tokenId << ": " << balance;
    first = false;
  }
  os << "}, publicKeys: [";
  bool firstKey = true;
  for (const auto& pk : wallet.publicKeys) {
    if (!firstKey) os << ", ";
    os << utl::toJsonSafeString(pk);
    firstKey = false;
  }
  os << "], minSignatures: " << (int)wallet.minSignatures
     << ", keyType: " << (int)wallet.keyType << "}";
  return os;
}

std::ostream& operator<<(std::ostream& os, const UserAccount& account) {
  os << "UserAccount{wallet: " << account.wallet << ", meta: \"" << account.meta << "\"}";
  return os;
}

pp::common::Meta Wallet::ltsToMeta() const {
  pp::common::Meta balances;
  for (const auto &[tokenId, balance] : mBalances) {
    balances.set(std::to_string(tokenId), static_cast<int64_t>(balance));
  }
  std::vector<pp::common::Meta::Value> keys;
  keys.reserve(publicKeys.size());
  for (const auto &pk : publicKeys) {
    // Named local avoids GCC -Wmaybe-uninitialized false positive on
    // std::variant move into vector from a converting temporary.
    std::string safe = utl::toJsonSafeString(pk);
    keys.emplace_back(std::move(safe));
  }
  pp::common::Meta j;
  j.set("mBalances", balances);
  j.set("publicKeys", pp::common::Meta::array(std::move(keys)));
  j.setJsonUInt("minSignatures", minSignatures);
  j.setJsonUInt("keyType", keyType);
  if (miner != MinerState{}) {
    pp::common::Meta m;
    m.set("registered", miner.registered);
    m.setJsonUInt("lastProducedSlot", miner.lastProducedSlot);
    m.setJsonUInt("missedDue", miner.missedDue);
    if (miner.autoDeregisteredEpoch != MinerState::kNever) {
      m.setJsonUInt("autoDeregisteredEpoch", miner.autoDeregisteredEpoch);
    }
    j.set("miner", m);
  }
  if (recovery != AccountRecovery{}) {
    auto keysOf = [](const std::vector<std::string> &list) {
      std::vector<pp::common::Meta::Value> out;
      for (const auto &pk : list) {
        std::string safe = utl::toJsonSafeString(pk);
        out.emplace_back(std::move(safe));
      }
      return out;
    };
    pp::common::Meta r;
    r.set("publicKeys", pp::common::Meta::array(keysOf(recovery.publicKeys)));
    r.setJsonUInt("minSignatures", recovery.minSignatures);
    r.setJsonUInt("delaySeconds", recovery.delaySeconds);
    if (recovery.pending) {
      pp::common::Meta p;
      p.set("publicKeys", pp::common::Meta::array(keysOf(recovery.nextPublicKeys)));
      p.setJsonUInt("minSignatures", recovery.nextMinSignatures);
      p.setJsonUInt("applyEpoch", recovery.applyEpoch);
      p.setJsonUInt("announcedAt", recovery.announcedAt);
      r.set("pending", p);
    }
    j.set("recovery", r);
  }
  if (!domain.empty()) {
    j.set("domain", domain);
  }
  return j;
}

std::string AccountRecoveryAction::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool AccountRecoveryAction::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  return !ar.failed();
}

std::string UserAccount::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar & VERSION & *this;
  return oss.str();
}

bool UserAccount::ltsFromString(const std::string& str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar & version;
  if (version != VERSION) {
    return false;
  }
  ar & *this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

pp::common::Meta UserAccount::ltsToMeta() const {
  pp::common::Meta j;
  j.set("wallet", wallet.ltsToMeta());
  j.set("meta", utl::toJsonSafeString(meta));
  return j;
}

} // namespace pp
