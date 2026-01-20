#pragma once

#include "DirDirStore.h"
#include "Block.h"
#include "Module.h"
#include "ResultOrError.hpp"

#include <vector>
#include <cstdint>
#include <string>

namespace pp {

class Ledger : public Module {
public:
  struct Transaction {
    std::string fromWallet; // Source wallet ID
    std::string toWallet;   // Destination wallet ID
    int64_t amount{ 0 };    // Transfer amount

    Transaction() = default;
  
    template <typename Archive> void serialize(Archive &ar) {
      ar &fromWallet &toWallet &amount;
    }
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  Ledger() { setLogger("ledger"); }
  virtual ~Ledger() = default;

  struct Config {
    std::string workDir;
    uint64_t startingBlockId{ 0 };
  };

  uint64_t getCurrentBlockId() const;

  Roe<void> init(const Config& config);
  Roe<void> addBlock(const Block& block);
  Roe<void> updateCheckpoints(const std::vector<uint64_t>& blockIds);

private:
  /**
   * Index file header structure - minimal header
   */
  struct IndexFileHeader {
    static constexpr uint32_t MAGIC = 0x504C4C44; // "PLLD" (PP Ledger Ledger Data)
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint32_t magic{ MAGIC };
    uint16_t version{ CURRENT_VERSION };

    IndexFileHeader() = default;

    template <typename Archive> void serialize(Archive &ar) {
      ar &magic &version;
    }
  };

  /**
   * Metadata structure - holds ledger metadata
   */
  struct Meta {
    uint64_t startingBlockId{ 0 };
    std::vector<uint64_t> checkpointIds;

    Meta() = default;

    template <typename Archive> void serialize(Archive &ar) {
      ar &startingBlockId &checkpointIds;
    }
  };

  std::string workDir_;
  std::string dataDir_;
  std::string indexFilePath_;
  Meta meta_;
  DirDirStore store_;

  bool loadIndex();
  bool saveIndex();
  Roe<void> cleanupData();
};

} // namespace pp