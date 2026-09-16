#ifndef PP_LEDGER_BLOCK_VALIDATION_H
#define PP_LEDGER_BLOCK_VALIDATION_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "Types.h"
#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "RecordHandler.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pp::chain_block {

/**
 * How thoroughly to admit a block before apply / persist.
 * See docs/architecture/BLOCK_PIPELINE.md.
 */
enum class BlockAdmissionMode : uint8_t {
  /** Tip ingest and seal policy: structural + consensus + body. */
  Full = 0,
  /** Replay from a checkpoint start: structural only; soft tx apply. */
  CheckpointReplay = 1,
  /** Persist after seal: structural only (effects already on tip bank). */
  SealedCommitVerify = 2,
};

constexpr bool admissionRunsConsensusAndBody(BlockAdmissionMode mode) {
  return mode == BlockAdmissionMode::Full;
}

/** Tx handlers use strict fee/idempotency/signature rules under Full only. */
constexpr bool admissionTxStrict(BlockAdmissionMode mode) {
  return mode == BlockAdmissionMode::Full;
}

/** Block hash: SHA-256 of header LTS only (records committed via txRoot). */
std::string calculateBlockHash(const Ledger::Block &block);

/** SHA-256 commitment to ordered records. Domain: "pp-ledger/txroot/v1". */
std::string calculateTxRoot(const std::vector<Ledger::Record> &records);

/**
 * SHA-256 commitment to the stakeholder set used for leader election.
 * Domain: "pp-ledger/stake/v1"; stakeholders packed sorted by id.
 */
std::string calculateStakeSnapshotHash(
    const std::vector<consensus::Stakeholder> &stakeholders);

/** SHA-256("pp-ledger/epoch-seed/genesis-config/v1" || LTS(config)). */
std::string calculateGenesisConfigDigest(const BlockChainConfig &config);

chain_tx::Roe<void> validateGenesisBlock(const Ledger::ChainNode &block,
                                        const RecordHandler &recordHandler);

chain_tx::Roe<void> validateBlockSequence(const Ledger &ledger,
                                          const Ledger::ChainNode &block);

chain_tx::Roe<void> validateIntraBlockIdempotency(
    const Ledger::ChainNode &block, const RecordHandler &recordHandler);

uint64_t getBlockAgeSeconds(uint64_t blockId, const Ledger &ledger,
                            const consensus::SlotCommittee &consensus);

bool needsCheckpoint(const BlockChainConfig &config, const Checkpoint &checkpoint,
                     uint64_t nextBlockId,
                     uint64_t checkpointBlockAgeSeconds);

chain_tx::Roe<uint64_t> calculateMaxBlockIdForRenewal(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, uint64_t atBlockId);

chain_tx::Roe<void> validateAccountRenewals(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler);

/**
 * Empty-body policy from genesis `heartbeatSlots`.
 * Non-empty blocks always pass. Empty blocks require tip lag ≥ threshold;
 * `heartbeatSlots == 0` rejects all empty bodies.
 */
chain_tx::Roe<void> validateEmptyHeartbeatPolicy(const Ledger::ChainNode &block,
                                                 uint64_t tipSlot,
                                                 const BlockChainConfig &config);

/** Layer: txRoot, header hash, sequence / previousHash / txIndex. */
chain_tx::Roe<void> checkBlockStructural(const Ledger::ChainNode &block,
                                         const Ledger &ledger);

/**
 * Layer: epoch, stake snapshot, epochSeed, slot leader, slot timing.
 * Requires consensus stake/seed already installed for the block's epoch.
 */
chain_tx::Roe<void>
checkBlockConsensus(const Ledger::ChainNode &block,
                    const consensus::SlotCommittee &consensus);

/**
 * Layer: renewals, maxTx, empty heartbeat, intra-block idempotency.
 * Requires chain config in Full tip contexts.
 */
chain_tx::Roe<void> checkBlockBodyPolicy(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler);

/**
 * Mode-selected admission check (no bank mutation).
 * Full = structural + consensus + body; other modes = structural only.
 */
chain_tx::Roe<void>
checkBlock(const Ledger::ChainNode &block, BlockAdmissionMode mode,
           const Ledger &ledger, const consensus::SlotCommittee &consensus,
           const AccountBuffer &bank,
           const std::optional<BlockChainConfig> &optChainConfig,
           const Checkpoint &checkpoint, const RecordHandler &recordHandler);

/**
 * Compatibility wrapper: strict → Full, else CheckpointReplay.
 * Prefer `checkBlock` at new call sites.
 */
chain_tx::Roe<void>
validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode,
                    const Ledger &ledger, const consensus::SlotCommittee &consensus,
                    const AccountBuffer &bank,
                    const std::optional<BlockChainConfig> &optChainConfig,
                    const Checkpoint &checkpoint,
                    const RecordHandler &recordHandler);

} // namespace pp::chain_block

#endif
