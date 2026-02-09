#pragma once

#include "DirDirStore.h"
#include "Module.h"
#include "ResultOrError.hpp"
#include "Utilities.h"

#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <nlohmann/json.hpp>

namespace pp {

class Ledger : public Module {
public:
  struct Transaction {
    constexpr static uint16_t T_DEFAULT = 0;
    constexpr static uint16_t T_CHECKPOINT = 1;
    constexpr static uint16_t T_USER = 2;

    uint16_t type{ T_DEFAULT };
    uint64_t tokenId{ 0 };      // Token ID (0 = native token)
    uint64_t fromWalletId{ 0 }; // Source wallet ID
    uint64_t toWalletId{ 0 };   // Destination wallet ID
    int64_t amount{ 0 };        // Transfer amount
    int64_t fee{ 0 };           // Transaction fee, always in native token, always goes to system fee account
    std::string meta;           // Transaction metadata

    template <typename Archive> void serialize(Archive &ar) {
      ar & type & tokenId& fromWalletId & toWalletId & amount & fee & meta;
    }

    nlohmann::json toJson() const;
  };

  template <typename T>
  struct SignedData {
    T obj;
    std::vector<std::string> signatures;

    template <typename Archive> void serialize(Archive &ar) {
      ar & obj & signatures;
    }

    nlohmann::json toJson() const {
      nlohmann::json j;
      j["object"] = obj.toJson();
      // Convert binary signatures to JSON-safe hex strings
      nlohmann::json sigArray = nlohmann::json::array();
      for (const auto& sig : signatures) {
        sigArray.push_back(utl::toJsonSafeString(sig));
      }
      j["signatures"] = sigArray;
      return j;
    }
  };

  /**
   * Block data structure (without hash)
   */
  struct Block {
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint64_t index{ 0 };
    int64_t timestamp{ 0 };
    std::vector<SignedData<Transaction>> signedTxes;
    std::string previousHash;
    uint64_t nonce{ 0 };
    uint64_t slot{ 0 };
    uint64_t slotLeader{ 0 };

    template <typename Archive> void serialize(Archive &ar) {
      ar & index & timestamp & signedTxes & previousHash & nonce & slot & slotLeader;
    }

    std::string ltsToString() const;
    bool ltsFromString(const std::string &str);
    nlohmann::json toJson() const;

  };

  /**
   * ChainNode data structure (Block + hash)
   * Simple struct for in-memory representation
   */
  struct ChainNode {
    Block block;
    std::string hash;

    /**
     * Serialize to binary (same format as stored on disk).
     * For network transport, hex-encode the result.
     */
    std::string ltsToString() const;

    /**
     * Deserialize from binary string.
     * @param str Output of ltsToString() (or hex-decode of wire format).
     * @return true if successful
     */
    bool ltsFromString(const std::string& str);

    nlohmann::json toJson() const;
  };

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  Ledger();
  virtual ~Ledger() = default;

  struct InitConfig {
    std::string workDir;
    uint64_t startingBlockId{ 0 };
  };

  uint64_t getNextBlockId() const;

  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const std::string& workDir);
  Roe<void> addBlock(const ChainNode& block);
  Roe<void> updateCheckpoints(const std::vector<uint64_t>& blockIds);
  Roe<ChainNode> readBlock(uint64_t blockId) const;
  Roe<ChainNode> readLastBlock() const;
  uint64_t countSizeFromBlockId(uint64_t blockId) const;

private:
  /**
   * RawBlock data structure for file storage
   * Stores block as serialized string to be version-agnostic
   */
  struct RawBlock {
    std::string data;  // Serialized Block data (from Block::ltsToString())
    std::string hash;

    template <typename Archive> void serialize(Archive &ar) {
      ar & data & hash;
    }
  };

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