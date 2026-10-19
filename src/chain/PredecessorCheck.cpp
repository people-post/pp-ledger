#include "ledger/chain/PredecessorCheck.h"

#include "ledger/chain/BlockValidation.h"
#include "ledger/chain/Types.h"
#include "ledger/lib/common/BinaryPack.hpp"
#include "ledger/lib/common/Utilities.h"

#include <filesystem>

namespace pp::predecessor_check {

namespace {

/** The genesis meta a block opens with: T_GENESIS at block 0, a genesis renewal at a checkpoint. */
std::optional<GenesisAccountMeta> genesisMetaOf(const Ledger::ChainNode &block) {
  if (block.block.records.empty()) {
    return std::nullopt;
  }
  const auto &record = block.block.records.front();
  std::string meta;
  if (record.type == Ledger::T_GENESIS) {
    auto tx = utl::binaryUnpack<Ledger::TxGenesis>(record.data);
    if (!tx) {
      return std::nullopt;
    }
    meta = tx.value().meta;
  } else if (chain_block::isGenesisRenewal(record)) {
    auto tx = utl::binaryUnpack<Ledger::TxRenewal>(record.data);
    if (!tx) {
      return std::nullopt;
    }
    meta = tx.value().meta;
  } else {
    return std::nullopt;
  }
  GenesisAccountMeta gm;
  if (!gm.ltsFromString(meta)) {
    return std::nullopt;
  }
  return gm;
}

} // namespace

pp::Roe<void> check(const Ledger::ChainNode &genesis, const std::string &oldWorkDir) {
  const auto gm = genesis.block.index == 0 ? genesisMetaOf(genesis) : std::nullopt;
  if (!gm || !gm->hasImport) {
    return pp::Error("This network's genesis imports no predecessor, but predecessorDir is set: a re-genesis was "
                     "expected — do not follow it");
  }
  const auto &predecessor = gm->import.predecessor;
  const std::string ledgerDir = (std::filesystem::path(oldWorkDir) / "data" / "ledger").string();
  Ledger ledger;
  if (auto mounted = ledger.mount(ledgerDir); !mounted) {
    return pp::Error("Cannot read the old ledger in " + ledgerDir + ": " + mounted.error().message);
  }
  auto first = ledger.readBlock(ledger.getStartingBlockId());
  const auto oldMeta = first ? genesisMetaOf(first.value()) : std::nullopt;
  if (!oldMeta || oldMeta->config.networkId != predecessor.networkId) {
    return pp::Error("The old ledger in " + ledgerDir + " is not of network '" + predecessor.networkId +
                     "', which the re-genesis names");
  }
  if (predecessor.height < ledger.getStartingBlockId() || predecessor.height >= ledger.getNextBlockId()) {
    return pp::Error("The old ledger does not hold block " + std::to_string(predecessor.height) +
                     " the re-genesis starts from: this node cannot vouch for it");
  }
  auto block = ledger.readBlock(predecessor.height);
  if (!block) {
    return pp::Error("Cannot read block " + std::to_string(predecessor.height) + " of the old ledger: " +
                     block.error().message);
  }
  if (block.value().hash != predecessor.blockHash || block.value().block.stateRoot != predecessor.stateRoot) {
    return pp::Error("Block " + std::to_string(predecessor.height) + " of your old ledger is not the one the "
                     "re-genesis starts from (block hash " + utl::hexEncode(predecessor.blockHash) +
                     "): do not follow it");
  }
  return {};
}

} // namespace pp::predecessor_check
