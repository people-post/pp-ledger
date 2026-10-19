#include "ledger/chain/MinerLiveness.h"
#include "ledger/chain/ErrorCodes.h"

#include <algorithm>
#include <string>

namespace pp::chain_tx {

bool isMinerDue(const Client::MinerState &miner, uint64_t slot, const BlockChainConfig &config) {
  return miner.registered && config.minerDueAfterSlots > 0 && slot >= miner.lastProducedSlot &&
         slot - miner.lastProducedSlot >= config.minerDueAfterSlots;
}

namespace {

/** One skipped slot led by `leaderId`: a miss if it was due; deregister at the limit. */
Roe<void> chargeSkippedSlot(AccountBuffer &bank, uint64_t leaderId, uint64_t slot, uint64_t blockEpoch,
                            const BlockChainConfig &config, bool strict, std::vector<uint64_t> *charged) {
  auto account = bank.getAccount(leaderId);
  if (!account) {
    return strict ? Roe<void>(TxError(chain_err::E_ACCOUNT_NOT_FOUND,
                                      "Slot leader account not found: " + std::to_string(leaderId)))
                  : Roe<void>();
  }
  Client::MinerState miner = account.value().wallet.miner;
  if (!isMinerDue(miner, slot, config)) {
    return {};
  }
  ++miner.missedDue;
  if (charged) {
    charged->push_back(leaderId);
  }
  if (config.minerMaxMissedSlots > 0 && miner.missedDue >= config.minerMaxMissedSlots) {
    miner.registered = false;
    miner.missedDue = 0;
    miner.autoDeregisteredEpoch = blockEpoch;
  }
  if (auto set = bank.setMinerState(leaderId, miner); !set) {
    return TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace

Roe<void> applyMinerLiveness(AccountBuffer &bank, const consensus::SlotCommittee &consensus,
                             const BlockChainConfig &config, uint64_t prevSlot, const Ledger::Block &block,
                             bool strict, bool chargeSkipped, std::vector<uint64_t> *charged) {
  // 1. Skipped slots of this block's epoch: its committee and seed elect their leaders.
  const uint64_t slotsPerEpoch = consensus.getConfig().slotsPerEpoch;
  const uint64_t epochFirstSlot = slotsPerEpoch > 0 ? block.epoch * slotsPerEpoch : block.slot;
  if (chargeSkipped && config.minerDueAfterSlots > 0) {
    for (uint64_t slot = std::max(prevSlot + 1, epochFirstSlot); slot < block.slot; ++slot) {
      auto leader = consensus.getSlotLeader(slot);
      if (!leader) {
        continue;  // no committee for the slot: nobody was elected
      }
      if (auto ok = chargeSkippedSlot(bank, leader.value(), slot, block.epoch, config, strict, charged); !ok) {
        return ok;
      }
    }
  }

  // 2. The producer showed up.
  auto producer = bank.getAccount(block.slotLeader);
  if (!producer) {
    return strict ? Roe<void>(TxError(chain_err::E_ACCOUNT_NOT_FOUND,
                                      "Block producer account not found: " + std::to_string(block.slotLeader)))
                  : Roe<void>();
  }
  Client::MinerState miner = producer.value().wallet.miner;
  if (!miner.registered || (miner.lastProducedSlot == block.slot && miner.missedDue == 0)) {
    return {};  // only a registered miner's clock matters (registration starts it afresh)
  }
  miner.lastProducedSlot = block.slot;
  miner.missedDue = 0;
  if (auto set = bank.setMinerState(block.slotLeader, miner); !set) {
    return TxError(chain_err::E_INTERNAL_BUFFER, set.error().message);
  }
  return {};
}

} // namespace pp::chain_tx
