#include "ledger/chain/StateSnapshot.h"
#include "ledger/chain/AccountBuffer.h"
#include "ledger/chain/AccountStateTree.h"
#include "ledger/chain/Chain.h"
#include "common/Serialize.hpp"

#include <filesystem>
#include <sstream>

namespace pp {

std::string StateSnapshot::ltsToString() const {
  std::ostringstream oss(std::ios::binary);
  OutputArchive ar(oss);
  ar &VERSION &*this;
  return oss.str();
}

bool StateSnapshot::ltsFromString(const std::string &str) {
  std::istringstream iss(str, std::ios::binary);
  InputArchive ar(iss);
  uint32_t version = 0;
  ar &version;
  if (version != VERSION) {
    return false;
  }
  ar &*this;
  return !ar.failed();
}

std::string StateSnapshot::accountsRoot() const {
  AccountStateTree tree;
  for (const auto &account : accounts) {
    AccountBuffer::Account leaf;
    leaf.id = account.id;
    leaf.wallet = account.wallet;
    leaf.blockId = account.blockId;
    leaf.attachmentHash = AccountBuffer::attachmentHashOf(account.attachment);
    AccountBuffer::setLeaves(tree, leaf);
  }
  return tree.root();
}

namespace state_export {

namespace {

/** The scratch directory, created by the export and removed whole however it ends. */
struct Scratch {
  std::filesystem::path dir;
  bool created{false};
  ~Scratch() {
    if (created) {
      std::error_code ec;
      std::filesystem::remove_all(dir, ec);
    }
  }
};

/** Copies `ledgerDir` into the scratch directory and cuts the copy after `height`. */
Roe<void> copyAndCut(const std::string &ledgerDir, uint64_t height, Scratch &scratch, const std::filesystem::path &copy) {
  std::error_code ec;
  if (std::filesystem::exists(scratch.dir, ec)) {
    return Error(1, "Scratch directory " + scratch.dir.string() + " exists: give a path that does not (it is removed after)");
  }
  if (!std::filesystem::create_directories(scratch.dir, ec) || ec) {
    return Error(1, "Cannot create scratch directory " + scratch.dir.string() + ": " + ec.message());
  }
  scratch.created = true;
  std::filesystem::copy(ledgerDir, copy, std::filesystem::copy_options::recursive, ec);
  if (ec) {
    return Error(1, "Cannot copy " + ledgerDir + " to " + copy.string() + ": " + ec.message());
  }
  Ledger ledger;
  if (auto mounted = ledger.mount(copy.string()); !mounted) {
    return Error(1, "Cannot mount the ledger in " + ledgerDir + ": " + mounted.error().message);
  }
  if (ledger.getStartingBlockId() != 0) {
    return Error(1, "The ledger in " + ledgerDir + " starts at block " + std::to_string(ledger.getStartingBlockId()) +
                        ": exporting needs a full node's ledger, from genesis (a relay's)");
  }
  if (height >= ledger.getNextBlockId()) {
    // What a node committed: export from a stopped node (a clean stop commits every block).
    return Error(1, ledger.getNextBlockId() == 0
                        ? std::string("The ledger holds no committed block")
                        : "The ledger's committed blocks end at block " + std::to_string(ledger.getNextBlockId() - 1) +
                              ", below height " + std::to_string(height));
  }
  if (auto cut = ledger.rewindTo(height + 1); !cut) {
    return Error(1, "Cannot cut the copy after block " + std::to_string(height) + ": " + cut.error().message);
  }
  return {};
}

} // namespace

Roe<StateSnapshot> exportState(const std::string &ledgerDir, uint64_t height, const std::string &scratchDir) {
  Scratch scratch{std::filesystem::path(scratchDir)};
  const auto copy = scratch.dir / "ledger";
  if (auto copied = copyAndCut(ledgerDir, height, scratch, copy); !copied) {
    return copied.error();
  }
  Chain chain;
  chain.redirectLogger("StateExport.Chain");
  consensus::SlotCommittee::Config consensusConfig;
  consensusConfig.timeOffsetMs = 0;
  chain.initConsensus(consensusConfig);
  if (auto mounted = chain.mountLedger(copy.string()); !mounted) {
    return Error(2, "Cannot mount the copy: " + mounted.error().message);
  }
  if (auto loaded = chain.loadFromLedger(0); !loaded) {
    return Error(2, "Cannot replay the chain to block " + std::to_string(height) + ": " + loaded.error().message);
  }
  auto block = chain.readBlock(height);
  if (!block) {
    return Error(2, "Cannot read block " + std::to_string(height) + ": " + block.error().message);
  }
  const auto config = chain.getChainConfig();
  if (!config) {
    return Error(2, "No chain config after the replay");
  }
  auto accounts = chain.exportAccounts();
  if (!accounts) {
    return Error(2, accounts.error().message);
  }

  StateSnapshot snapshot;
  snapshot.predecessor.networkId = chain.getNetworkId();
  snapshot.predecessor.height = height;
  snapshot.predecessor.blockHash = block.value().hash;
  snapshot.predecessor.stateRoot = block.value().block.stateRoot;
  snapshot.predecessor.header = block.value().block;
  snapshot.predecessor.header.records.clear(); // the header binds the root; the body is not needed
  snapshot.predecessor.header.committee.clear();
  snapshot.config = *config;
  snapshot.accounts = std::move(accounts.value());
  if (snapshot.accountsRoot() != snapshot.predecessor.stateRoot) {
    return Error(3, "The accounts do not hash to block " + std::to_string(height) + "'s state root");
  }
  return snapshot;
}

} // namespace state_export

} // namespace pp
