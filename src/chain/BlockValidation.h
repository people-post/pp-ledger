#ifndef PP_LEDGER_BLOCK_VALIDATION_H
#define PP_LEDGER_BLOCK_VALIDATION_H

#include "AccountBuffer.h"
#include "BlockAdmission.h"
#include "TxError.h"
#include "Types.h"
#include "../consensus/SlotCommittee.h"
#include "../ledger/Ledger.h"
#include "RecordHandler.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pp::chain_block {

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

/** Committee members sorted by id, as stakeSnapshotHash commits to them. */
std::vector<Ledger::CommitteeMember> toCommittee(const std::vector<consensus::Stakeholder> &stakeholders);
std::vector<consensus::Stakeholder> fromCommittee(const std::vector<Ledger::CommitteeMember> &committee);

/**
 * True when `block` is the first block of its epoch on this ledger — or the first block it holds
 * (a node starting at a checkpoint, which opens its epoch). It then carries its committee.
 */
bool opensEpoch(const Ledger &ledger, const Ledger::Block &block);

/**
 * The committee field (beacon-authority ba5b): an epoch's first block carries the members its
 * stakeSnapshotHash commits to; every other block carries none.
 */
chain_tx::Roe<void> validateBlockCommittee(const Ledger &ledger, const Ledger::Block &block);

/** A T_RENEWAL of the genesis account (it carries GenesisAccountMeta). */
bool isGenesisRenewal(const Ledger::Record &record);

/**
 * True when block `index` (of `epoch`) becomes the next checkpoint (beacon-authority ba5b): the
 * first block of an epoch, at least `checkpoint.minBlocks` after the current checkpoint, whose
 * previous block's slot starts more than `checkpoint.minAgeSeconds` after the current checkpoint's
 * block. Then every account last written before the current checkpoint renewed by the previous
 * block. Depends only on stored blocks and slot times, so every node agrees.
 */
bool rotatesCheckpoint(const Ledger &ledger, const consensus::SlotCommittee &consensus,
                       const BlockChainConfig &config, const Checkpoint &checkpoint, uint64_t index,
                       uint64_t epoch);

/**
 * The renewal cut-off for block `atBlockId` at `atSlot`: accounts last written before the returned
 * block id must renew in it (0 = none). Uses the block's slot time, never this node's clock.
 */
chain_tx::Roe<uint64_t> calculateMaxBlockIdForRenewal(
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, uint64_t atBlockId, uint64_t atSlot);

chain_tx::Roe<void> validateAccountRenewals(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::SlotCommittee &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler);

/**
 * Empty-body policy. Non-empty blocks always pass. An empty block passes when
 * its producer is a due miner (`producerDue`, see chain_tx::isMinerDue) or the
 * tip lag is at least `heartbeatSlots` (0 disables the lag rule).
 */
chain_tx::Roe<void> validateEmptyHeartbeatPolicy(const Ledger::ChainNode &block,
                                                 uint64_t tipSlot,
                                                 const BlockChainConfig &config,
                                                 bool producerDue);

/**
 * At most one `T_BEACON_CERT` and one `T_RECOVERY` per block (beacon-authority B011, B019): each
 * moves the authority the next one would be checked against.
 */
chain_tx::Roe<void> validateAuthorityRecordCount(const Ledger::Block &block);

/** Layer: txRoot, header hash, authority records per block, sequence / previousHash / txIndex. */
chain_tx::Roe<void> checkBlockStructural(const Ledger::ChainNode &block,
                                         const Ledger &ledger);

/**
 * Layer: epoch, stake snapshot, epochSeed, slot leader, slot timing.
 * Requires consensus stake/seed already installed for the block's epoch.
 */
chain_tx::Roe<void>
checkBlockConsensus(const Ledger::ChainNode &block,
                    const consensus::SlotCommittee &consensus);

/** Future-slot slack in slots: max(2 slots, 15 seconds). */
uint64_t futureSlotToleranceSlots(uint64_t slotDuration);

/**
 * Layer for newly arriving or freshly sealed blocks only (never ledger
 * reload / history replay): slot strictly above the previous block's slot
 * and not beyond the local current slot + futureSlotToleranceSlots.
 */
chain_tx::Roe<void>
checkBlockArrival(const Ledger::ChainNode &block, const Ledger &ledger,
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

} // namespace pp::chain_block

#endif
