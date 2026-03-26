#ifndef PP_LEDGER_BLOCK_VALIDATION_H
#define PP_LEDGER_BLOCK_VALIDATION_H

#include "AccountBuffer.h"
#include "TxError.h"
#include "Types.h"
#include "../consensus/Ouroboros.h"
#include "../ledger/Ledger.h"

#include <cstdint>
#include <optional>
#include <string>

namespace pp::chain_block {

/** Block hash: SHA-256 of binary LTS serialization (same as Chain::calculateHash). */
std::string calculateBlockHash(const Ledger::Block &block);

chain_tx::Roe<void> validateGenesisBlock(const Ledger::ChainNode &block);

chain_tx::Roe<void> validateBlockSequence(const Ledger &ledger,
                                          const Ledger::ChainNode &block);

chain_tx::Roe<void> validateIntraBlockIdempotency(const Ledger::ChainNode &block);

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
    const Checkpoint &checkpoint);

chain_tx::Roe<void>
validateNormalBlock(const Ledger::ChainNode &block, bool isStrictMode,
                    const Ledger &ledger, const consensus::Ouroboros &consensus,
                    const AccountBuffer &bank,
                    const std::optional<BlockChainConfig> &optChainConfig,
                    const Checkpoint &checkpoint);

} // namespace pp::chain_block

#endif
