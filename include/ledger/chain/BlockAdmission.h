#ifndef PP_LEDGER_BLOCK_ADMISSION_H
#define PP_LEDGER_BLOCK_ADMISSION_H

#include <cstdint>

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

} // namespace pp::chain_block

#endif
