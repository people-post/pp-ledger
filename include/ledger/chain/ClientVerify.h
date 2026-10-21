#ifndef PP_LEDGER_CLIENT_VERIFY_H
#define PP_LEDGER_CLIENT_VERIFY_H

// What a client — a wallet — checks so that no node need be believed (WIRE_SCHEMA § Account proofs,
// BEACON_AUTHORITY § Trust chain): from the genesis hash it pinned, block 0; from block 0 and an
// authority proof, the beacon key in force; from it, a signed tip; under the tip, blocks by their
// hashes; in a block's state root, an account by its Merkle path. Each step only on what the previous
// one proved.

#include "ledger/chain/AuthorityTrail.h"
#include "ledger/chain/TxError.h"
#include "ledger/client/Client.h"
#include "ledger/ledger/Ledger.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pp::verify {

/** Block 0 is the one `genesisHash` names: its hash recomputed from its contents. */
chain_tx::Roe<void> checkGenesis(const Ledger::ChainNode &genesis, const std::string &genesisHash);

/**
 * The beacon authority at block `upToBlockId` of `upToEpoch`: `proof` replayed from block 0 (checked
 * first), each step signed by the admin set in force at its point.
 */
chain_tx::Roe<AuthorityTrail> authorityAt(const Ledger::ChainNode &genesis, const std::string &genesisHash,
                                          const Client::AuthorityProof &proof, uint64_t upToBlockId,
                                          uint64_t upToEpoch);

/**
 * `blocks`, from the signed tip's block down (consecutive, the tip's first), are the chain the tip
 * signs: each hash recomputed from its contents, each linked to the next by `previousHash`, the first
 * the tip's height and hash. The tip's signature is the trail's to check (AuthorityTrail::verifyTip).
 */
chain_tx::Roe<void> checkBlocksUnderTip(const Client::SignedTip &tip, const std::vector<Ledger::ChainNode> &blocks);

/** An account as a proof shows it, once checked: none when the account does not exist. */
struct ProvenAccount {
  uint64_t accountId{0};
  uint64_t blockId{0};
  std::optional<Client::Wallet> wallet;
  uint64_t lastWriteBlockId{0};
  std::string attachmentHash;
};

/**
 * `proof` against `block` (checked already): its leaf and path give the block's state root. The
 * proof must be of `accountId`, the account asked for: a node chooses what it answers.
 */
chain_tx::Roe<ProvenAccount> checkAccountProof(const Ledger::Block &block, const Client::AccountProof &proof,
                                               uint64_t accountId);

/** A domain as a proof shows it, once checked (projects/domains): no account when nobody has it. */
struct ProvenDomain {
  std::string domain;
  uint64_t blockId{0};
  std::optional<uint64_t> accountId;
};

/**
 * `proof` against `block` (checked already): the domain's leaf and path give the block's state root.
 * The proof must be of `domain` (normalized), the domain asked for.
 */
chain_tx::Roe<ProvenDomain> checkDomainProof(const Ledger::Block &block, const Client::DomainProof &proof,
                                             const std::string &domain);

} // namespace pp::verify

#endif // PP_LEDGER_CLIENT_VERIFY_H
