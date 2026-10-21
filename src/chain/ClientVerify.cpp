#include "ledger/chain/ClientVerify.h"
#include "ledger/chain/Domains.h"

#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountStateTree.h"
#include "chain/BlockValidation.h"
#include "ledger/chain/ErrorCodes.h"

namespace pp::verify {

namespace {

chain_tx::TxError refused(const std::string &message) { return chain_tx::TxError(chain_err::E_TX_VALIDATION, message); }

} // namespace

chain_tx::Roe<void> checkGenesis(const Ledger::ChainNode &genesis, const std::string &genesisHash) {
  if (genesis.block.index != 0) {
    return refused("Not block 0");
  }
  if (chain_block::calculateBlockHash(genesis.block) != genesis.hash || genesis.hash != genesisHash) {
    return refused("Block 0 is not the one the pinned genesis hash names");
  }
  return {};
}

chain_tx::Roe<AuthorityTrail> authorityAt(const Ledger::ChainNode &genesis, const std::string &genesisHash,
                                          const Client::AuthorityProof &proof, uint64_t upToBlockId,
                                          uint64_t upToEpoch) {
  if (auto ok = checkGenesis(genesis, genesisHash); !ok) {
    return ok.error();
  }
  return AuthorityTrail::replayProof(genesis.block, proof, upToBlockId, upToEpoch);
}

chain_tx::Roe<void> checkBlocksUnderTip(const Client::SignedTip &tip, const std::vector<Ledger::ChainNode> &blocks) {
  if (blocks.empty()) {
    return refused("No blocks to check");
  }
  if (blocks.front().block.index != tip.height || blocks.front().hash != tip.blockHash) {
    return refused("The first block is not the signed tip's");
  }
  for (size_t i = 0; i < blocks.size(); ++i) {
    const auto &node = blocks[i];
    if (chain_block::calculateBlockHash(node.block) != node.hash) {
      return refused("Block " + std::to_string(node.block.index) + " does not hash to its hash");
    }
    if (i + 1 < blocks.size()) {
      const auto &below = blocks[i + 1];
      if (below.block.index + 1 != node.block.index || node.block.previousHash != below.hash) {
        return refused("Block " + std::to_string(below.block.index) + " is not the one below block " +
                       std::to_string(node.block.index));
      }
    }
  }
  return {};
}

namespace {

/** A proof's path: the siblings carried, the empty subtree's where the mask leaves them out. */
chain_tx::Roe<std::vector<std::string>> unpackPath(uint64_t mask, const std::vector<std::string> &carried) {
  std::vector<std::string> siblings(AccountStateTree::kDepth);
  size_t next = 0;
  for (int h = 0; h < AccountStateTree::kDepth; ++h) {
    if ((mask >> h) & 1ULL) {
      if (next >= carried.size()) {
        return refused("The proof's path is short");
      }
      siblings[static_cast<size_t>(h)] = carried[next++];
    } else {
      siblings[static_cast<size_t>(h)] = AccountStateTree::emptySubtree(h);
    }
  }
  if (next != carried.size()) {
    return refused("The proof's path is long");
  }
  return siblings;
}

} // namespace

chain_tx::Roe<ProvenAccount> checkAccountProof(const Ledger::Block &block, const Client::AccountProof &proof,
                                               uint64_t accountId) {
  if (proof.accountId != accountId) {
    return refused("The proof is of account " + std::to_string(proof.accountId) + ", not " +
                   std::to_string(accountId));
  }
  if (proof.blockId != block.index || proof.stateRoot != block.stateRoot) {
    return refused("The proof is of another block");
  }
  if (proof.accountId >= AccountIds::ID_LIMIT) {
    return refused("Not an account id: the state tree's keys from 2^63 up are domains'");
  }
  auto siblings = unpackPath(proof.siblingMask, proof.siblings);
  if (!siblings) {
    return siblings.error();
  }
  std::string leaf = AccountStateTree::emptyLeaf();
  if (proof.present) {
    AccountBuffer::Account account;
    account.id = proof.accountId;
    account.wallet = proof.wallet;
    account.blockId = proof.lastWriteBlockId;
    account.attachmentHash = proof.attachmentHash;
    leaf = AccountBuffer::accountLeafHash(account);
  }
  if (AccountStateTree::rootFrom(proof.accountId, leaf, siblings.value()) != block.stateRoot) {
    return refused("The account is not in the block's state root as the proof says");
  }
  ProvenAccount out;
  out.accountId = proof.accountId;
  out.blockId = block.index;
  if (proof.present) {
    out.wallet = proof.wallet;
    out.lastWriteBlockId = proof.lastWriteBlockId;
    out.attachmentHash = proof.attachmentHash;
  }
  return out;
}

chain_tx::Roe<ProvenDomain> checkDomainProof(const Ledger::Block &block, const Client::DomainProof &proof,
                                             const std::string &domain) {
  if (proof.domain != domain) {
    return refused("The proof is of domain " + proof.domain + ", not " + domain);
  }
  if (proof.blockId != block.index || proof.stateRoot != block.stateRoot) {
    return refused("The proof is of another block");
  }
  if (!chain_domain::isWellFormed(proof.domain)) {
    return refused("Not a domain: " + proof.domain);
  }
  if (proof.present && proof.accountId >= AccountIds::ID_LIMIT) {
    return refused("Not an account id");
  }
  auto siblings = unpackPath(proof.siblingMask, proof.siblings);
  if (!siblings) {
    return siblings.error();
  }
  const std::string leaf =
      proof.present ? chain_domain::leafHash(proof.domain, proof.accountId) : AccountStateTree::emptyLeaf();
  if (AccountStateTree::rootFrom(chain_domain::treeKey(proof.domain), leaf, siblings.value()) != block.stateRoot) {
    return refused("The domain is not in the block's state root as the proof says");
  }
  ProvenDomain out;
  out.domain = proof.domain;
  out.blockId = block.index;
  if (proof.present) {
    out.accountId = proof.accountId;
  }
  return out;
}

} // namespace pp::verify
