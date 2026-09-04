#ifndef PP_LEDGER_BLOCK_VALIDATION_H
#define PP_LEDGER_BLOCK_VALIDATION_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "Types.h"
#include "../consensus/Ouroboros.h"
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

chain_tx::Roe<void> validateGenesisBlock(const Ledger::ChainNode &block,
                                        const RecordHandler &recordHandler);

chain_tx::Roe<void> validateBlockSequence(const Ledger &ledger,
                                          const Ledger::ChainNode &block);

chain_tx::Roe<void> validateIntraBlockIdempotency(
    const Ledger::ChainNode &block, const RecordHandler &recordHandler);

uint64_t getBlockAgeSeconds(uint64_t blockId, const Ledger &ledger,
                            const consensus::Ouroboros &consensus);

bool needsCheckpoint(const BlockChainConfig &config, const Checkpoint &checkpoint,
                     uint64_t nextBlockId,
                     uint64_t checkpointBlockAgeSeconds);

chain_tx::Roe<uint64_t> calculateMaxBlockIdForRenewal(
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, uint64_t atBlockId);

chain_tx::Roe<void> validateAccountRenewals(
    const Ledger::ChainNode &block, const AccountBuffer &bank,
    const Ledger &ledger, const consensus::Ouroboros &consensus,
    const std::optional<BlockChainConfig> &optChainConfig,
    const Checkpoint &checkpoint, const RecordHandler &recordHandler);

chain_tx::Roe<void>
validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode,
                    const Ledger &ledger, const consensus::Ouroboros &consensus,
                    const AccountBuffer &bank,
                    const std::optional<BlockChainConfig> &optChainConfig,
                    const Checkpoint &checkpoint,
                    const RecordHandler &recordHandler);

} // namespace pp::chain_block

#endif
