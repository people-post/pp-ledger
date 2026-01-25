#pragma once

#include "DirDirStore.h"
#include "Module.h"
#include "ResultOrError.hpp"

#include <vector>
#include <cstdint>
#include <string>
#include <memory>

namespace pp {

class Ledger : public Module {
public:
  /**
   * Block data structure (without hash)
   */
  struct Block {
    static constexpr uint16_t CURRENT_VERSION = 1;

    Block();

    // Core block methods
    std::string calculateHash() const;

    // Public fields
    uint64_t index{ 0 };
    int64_t timestamp{ 0 };
    std::string data;
    std::string previousHash;
    uint64_t nonce{ 0 };
    uint64_t slot{ 0 };
    std::string slotLeader;

    template <typename Archive> void serialize(Archive &ar) {
      ar & index & timestamp & data & previousHash & nonce & slot & slotLeader;
    }
  };

  /**
   * RawBlock data structure (Block + hash)
   */
  struct RawBlock {
    static constexpr uint16_t CURRENT_VERSION = 1;

    RawBlock();

    // Long-term support serialization for disk persistence
    /**
     * Serialize block to binary format for long-term storage
     * Format is version-aware and compact for efficient disk storage
     * Binary format:
     * [version][block][hash_size+hash]
     * @return Serialized binary string representation
     */
    std::string ltsToString() const;

    /**
     * Deserialize block from binary format
     * @param str Serialized binary string representation
     * @return true if successful, false on error
     */
    bool ltsFromString(const std::string &str);

    // Public fields
    Block block;
    std::string hash;

    template <typename Archive> void serialize(Archive &ar) {
      ar & block & hash;
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
  Roe<void> addBlock(const RawBlock& block);
  Roe<void> updateCheckpoints(const std::vector<uint64_t>& blockIds);
  Roe<RawBlock> readBlock(uint64_t blockId) const;

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