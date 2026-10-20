#include "ledger/chain/Types.h"
#include "ledger/lib/common/Utilities.h"

#include <sstream>

namespace pp {

std::ostream &operator<<(std::ostream &os, const CheckpointConfig &config) {
  os << "CheckpointConfig{minBlocks: " << config.minBlocks
     << ", minAgeSeconds: " << config.minAgeSeconds << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os, const BlockChainConfig &config) {
  os << "BlockChainConfig{genesisTime: " << config.genesisTime << ", "
     << "slotDuration: " << config.slotDuration << ", "
     << "slotsPerEpoch: " << config.slotsPerEpoch << ", "
     << "maxCustomMetaSize: " << config.maxCustomMetaSize << ", "
     << "maxTransactionsPerBlock: " << config.maxTransactionsPerBlock << ", "
     << "minFeeCoefficients: [" << utl::join(config.minFeeCoefficients, ", ")
     << "], "
     << "freeCustomMetaSize: " << config.freeCustomMetaSize << ", "
     << "checkpoint: " << config.checkpoint << ", "
     << "maxValidationTimespanSeconds: " << config.maxValidationTimespanSeconds
     << ", "
     << "networkId: " << config.networkId << ", "
     << "heartbeatSlots: " << config.heartbeatSlots << ", "
     << "maxIssuancePerEpoch: " << config.maxIssuancePerEpoch << ", "
     << "newAccountFee: " << config.newAccountFee << ", "
     << "minerMinStake: " << config.minerMinStake << ", "
     << "minerRegistrationFee: " << config.minerRegistrationFee << ", "
     << "minerDueAfterSlots: " << config.minerDueAfterSlots << ", "
     << "minerMaxMissedSlots: " << config.minerMaxMissedSlots << ", "
     << "minerReregisterCooldownEpochs: " << config.minerReregisterCooldownEpochs << ", "
     << "maxBlockBytes: " << config.maxBlockBytes << ", "
     << "finalitySeconds: " << config.finalitySeconds << "}";
  return os;
}

std::ostream &operator<<(std::ostream &os, const ChainAuthority &authority) {
  os << "ChainAuthority{recovery: {keys: " << authority.recovery.publicKeys.size()
     << ", minSignatures: " << static_cast<int>(authority.recovery.minSignatures)
     << ", delayEpochs: " << authority.recovery.delayEpochs << "}, beacon: {networkId: "
     << authority.beacon.networkId << ", generation: " << authority.beacon.generation
     << ", cutoverHeight: " << authority.beacon.cutoverHeight << "}";
  if (authority.hasPendingRecovery) {
    os << ", pendingRecovery: {adminKeys: " << authority.pendingRecovery.announcement.adminPublicKeys.size()
       << ", adminMinSignatures: " << static_cast<int>(authority.pendingRecovery.announcement.adminMinSignatures)
       << ", announcedAt: " << authority.pendingRecovery.announcedAt
       << ", applyEpoch: " << authority.pendingRecovery.applyEpoch << "}";
  }
  os << "}";
  return os;
}

std::string RecoveryAnnouncement::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint32_t version = VERSION;
  RecoveryAnnouncement copy = *this;
  ar &version &copy;
  return oss.str();
}

bool RecoveryAnnouncement::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  return !ar.failed();
}

std::string BeaconCertificate::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  uint32_t version = VERSION;
  BeaconCertificate copy = *this;
  ar &version &copy;
  return oss.str();
}

bool BeaconCertificate::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  return !ar.failed();
}

bool ChainAuthority::operator==(const ChainAuthority &other) const {
  auto pack = [](const ChainAuthority &a) {
    std::ostringstream oss(std::ios::binary);
    OutputArchive ar(oss);
    ChainAuthority copy = a;
    ar &copy;
    return oss.str();
  };
  return pack(*this) == pack(other);
}

void GenesisAccountMeta::setConfigStateAt(const BlockChainConfig &active,
                                          const std::optional<PendingChainConfig> &pendingUpdate,
                                          uint64_t epoch) {
  if (pendingUpdate.has_value() && epoch >= pendingUpdate->activationEpoch) {
    config = pendingUpdate->config;
    hasPending = false;
    pending = {};
    return;
  }
  config = active;
  hasPending = pendingUpdate.has_value();
  pending = pendingUpdate.value_or(PendingChainConfig{});
}

bool GenesisAccountMeta::sameConfigState(const GenesisAccountMeta &other) const {
  auto pack = [](const GenesisAccountMeta &gm) {
    std::ostringstream oss(std::ios::binary);
    OutputArchive ar(oss);
    BlockChainConfig config = gm.config;
    bool hasPending = gm.hasPending;
    PendingChainConfig pending = gm.pending;
    ar &config &hasPending &pending;
    return oss.str();
  };
  return pack(*this) == pack(other);
}

std::string GenesisAccountMeta::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar &VERSION &*this;
  return oss.str();
}

bool GenesisAccountMeta::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  if (ar.failed()) {
    return false;
  }
  return true;
}

} // namespace pp
