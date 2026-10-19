#ifndef PP_LEDGER_TX_CONTEXT_H
#define PP_LEDGER_TX_CONTEXT_H

#include "ledger/chain/BlockAdmission.h"
#include "ledger/chain/Types.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountStateTree.h"
#include "ledger/chain/TxFees.h"
#include "ledger/chain/TxIdempotency.h"
#include "ledger/consensus/SlotCommittee.h"
#include "ledger/ledger/Ledger.h"
#include "ledger/lib/common/Crypto.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pp {

/** Callables for scanning block records for serialized account metadata. */
struct FnAccountMetaForRecord {
  std::function<std::optional<std::string>(const Ledger::Record &, uint64_t)>
      fnUser;
  std::function<std::optional<std::string>(const Ledger::Record &,
                                             const Ledger::Block &)>
      fnGenesis;
};

/**
 * An importing genesis's span in progress (beacon-authority B032): set by its T_GENESIS, advanced
 * by each T_IMPORT, ended by the block that imports the last account — once the old leaves hash to
 * the predecessor's state root.
 */
struct ImportProgress {
  ChainPredecessor predecessor;
  uint64_t accountCount{0};
  uint64_t imported{0};
  std::optional<uint64_t> lastId;
  /** Keys the system accounts take (reserve, registrar, fee, recycle). */
  std::vector<SystemAccountKeys> systemKeys;
  /** The imported accounts' leaves as the old chain hashed them. */
  AccountStateTree oldLeaves;

  bool complete() const { return imported == accountCount; }
};

/**
 * Chain subsystem bundle passed to transaction handlers.
 * Chain owns a single TxContext member; handlers take TxContext & or
 * const TxContext & depending on whether they may mutate chain state.
 */
struct TxContext {
  Crypto crypto;
  consensus::SlotCommittee consensus;
  Ledger ledger;
  AccountBuffer bank;
  std::optional<BlockChainConfig> optChainConfig{std::nullopt};
  /** Accepted `T_CONFIG` not yet in force; Chain activates it at its epoch. */
  std::optional<PendingChainConfig> pendingChainConfig{std::nullopt};
  /** Recovery set + beacon certificate, set by the genesis record (beacon-authority). */
  std::optional<ChainAuthority> optAuthority{std::nullopt};
  /** Set by Chain at each epoch's first block; bounds issuance per epoch. */
  std::optional<IssuanceBaseline> issuanceBaseline{std::nullopt};
  Checkpoint checkpoint{};
  /** While an importing genesis's span is open (blocks 1..k); none otherwise. */
  std::optional<ImportProgress> importProgress{std::nullopt};
  std::optional<FnAccountMetaForRecord> fnAccountMetaForRecord{std::nullopt};
  std::optional<chain_tx::FnIdempotencyKeyForRecord> fnIdempotencyKeyForRecord{
      std::nullopt};
  /** What the idempotency scan keeps of each block (read and decoded once; lr8). */
  std::shared_ptr<chain_tx::IdempotencyScanCache> idempotencyScanCache{
      std::make_shared<chain_tx::IdempotencyScanCache>()};
  std::optional<chain_tx::FnBillableCustomMetaSizeForFee>
      fnBillableCustomMetaSizeForFee{std::nullopt};
};

/** Scratch-buffer / mempool path after signature validation. */
struct BufferApplyContext {
  const TxContext &ctx;
  uint64_t blockId{0};
  uint64_t effectiveSlot{0};
  chain_block::BlockAdmissionMode admissionMode{
      chain_block::BlockAdmissionMode::Full};
};

/** Committed-chain (block replay) path after signature validation. */
struct BlockApplyContext {
  TxContext &ctx;
  uint64_t blockId{0};
  uint64_t blockSlot{0};
  uint64_t slotLeaderId{0};
  chain_block::BlockAdmissionMode admissionMode{
      chain_block::BlockAdmissionMode::Full};
};

} // namespace pp

#endif
