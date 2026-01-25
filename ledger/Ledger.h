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
  struct TransactionData {
    std::string data;
    std::string signature;

    template <typename Archive> void serialize(Archive &ar) {
      ar &data &signature;
    }
  };

  struct BlockData {
    std::vector<TransactionData> transactions;

    template <typename Archive> void serialize(Archive &ar) {
      ar &transactions;
    }
  };

  struct Transaction {
    constexpr static uint16_t T_TRANSFER = 0;
    constexpr static uint16_t T_REGISTER = 1;

    uint16_t type{ T_TRANSFER };
    std::string fromWallet; // Source wallet ID
    std::string toWallet;   // Destination wallet ID
    int64_t amount{ 0 };    // Transfer amount
    std::string meta;       // Transaction metadata

    Transaction() = default;
  
    template <typename Archive> void serialize(Archive &ar) {
      ar &type &fromWallet &toWallet &amount &meta;
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

  uint64_t getNextBlockId() const;

  Roe<void> init(const Config& config);
  Roe<void> addBlock(const Block& block);
  Roe<void> updateCheckpoints(const std::vector<uint64_t>& blockIds);
  Roe<Block> readBlock(uint64_t blockId) const;

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