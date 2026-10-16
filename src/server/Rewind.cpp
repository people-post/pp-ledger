#include "Rewind.h"
#include "NodeFile.h"
#include "../ledger/Ledger.h"
#include "lib/common/FileSync.h"

#include <filesystem>

namespace pp::rewind {

namespace {

std::string pathIn(const std::string &dataDir, const char *file) { return dataDir + "/" + file; }

pp::Roe<std::optional<Client::RewindReport>> readReport(const std::string &path) {
  auto bytes = node_file::readIfExists(path);
  if (!bytes) {
    return pp::Error(1, bytes.error().message);
  }
  if (!bytes.value()) {
    return std::optional<Client::RewindReport>{};
  }
  Client::RewindReport report;
  if (!report.ltsFromString(*bytes.value())) {
    return pp::Error(1, path + " is not a rewind record");
  }
  return std::optional<Client::RewindReport>(report);
}

/** Cut the ledger at `ledgerDir` after `keptUpTo`, or remove it when that is below its first block. */
pp::Roe<void> rewindLedger(const std::string &ledgerDir, uint64_t keptUpTo) {
  if (!std::filesystem::exists(ledgerDir)) {
    return {};
  }
  bool joinFresh = false;
  {
    Ledger ledger;
    if (auto mounted = ledger.mount(ledgerDir); !mounted) {
      return pp::Error(1, "Failed to mount the ledger to rewind it: " + mounted.error().message);
    }
    if (keptUpTo + 1 < ledger.getStartingBlockId()) {
      joinFresh = true;
    } else if (ledger.getNextBlockId() > keptUpTo + 1) {
      if (auto rewound = ledger.rewindTo(keptUpTo + 1); !rewound) {
        return pp::Error(1, rewound.error().message);
      }
    }
  } // closed: Windows cannot remove open files
  if (joinFresh) {
    std::error_code ec;
    std::filesystem::remove_all(ledgerDir, ec);
    if (ec) {
      return pp::Error(1, "Failed to remove the ledger " + ledgerDir + ": " + ec.message());
    }
    const std::string parent = std::filesystem::path(ledgerDir).parent_path().string();
    if (auto synced = utl::syncDirectory(parent.empty() ? std::string(".") : parent); !synced) {
      return synced;
    }
  }
  return {};
}

} // namespace

pp::Roe<void> writeOrder(const std::string &dataDir, const Client::RewindReport &order) {
  if (auto written = node_file::replace(pathIn(dataDir, FILE_ORDER), order.ltsToString()); !written) {
    return pp::Error(1, written.error().message);
  }
  return {};
}

pp::Roe<std::optional<Client::RewindReport>> applyPendingOrder(const std::string &dataDir,
                                                               const std::string &ledgerDir) {
  auto order = readReport(pathIn(dataDir, FILE_ORDER));
  if (!order || !order.value()) {
    return order;
  }
  if (auto rewound = rewindLedger(ledgerDir, order.value()->keptUpTo); !rewound) {
    return rewound.error();
  }
  // Recorded before the order goes: a crash in between applies it again, which changes nothing.
  if (auto kept = node_file::replace(pathIn(dataDir, FILE_DONE), order.value()->ltsToString()); !kept) {
    return pp::Error(1, kept.error().message);
  }
  if (auto removed = node_file::remove(pathIn(dataDir, FILE_ORDER)); !removed) {
    return pp::Error(1, removed.error().message);
  }
  return order;
}

pp::Roe<std::optional<Client::RewindReport>> lastApplied(const std::string &dataDir) {
  return readReport(pathIn(dataDir, FILE_DONE));
}

} // namespace pp::rewind
