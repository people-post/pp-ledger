#pragma once

#include "DirDirStore.h"
#include "lib/common/Module.h"
#include "lib/common/ResultOrError.hpp"
#include "lib/common/Utilities.h"

#include <vector>
#include <cstdint>
#include <string>
#include <memory>
#include <optional>
#include <variant>
#include <json.hpp>

namespace pp {

class Ledger : public Module {
public:
  constexpr static uint16_t T_DEFAULT = 0;
  constexpr static uint16_t T_GENESIS = 1;     // Genesis account to initialize the system
  constexpr static uint16_t T_NEW_USER = 2;    // Any account to fund initialize a new user
  constexpr static uint16_t T_CONFIG = 3;      // Genesis account to update the system config
  constexpr static uint16_t T_USER_UPDATE = 4; // User accounts to update their account info
  constexpr static uint16_t T_RENEWAL = 5;     // Miner to renew user/genesis account with latest account info
  constexpr static uint16_t T_END_USER = 6;    // Miner to terminate user account due to insufficient fee.

  struct TxCommon {
    uint64_t fee{ 0 };          // Transaction fee, always in native token, always goes to system fee account
    std::string meta;           // Transaction metadata

    template <typename Archive> void serialize(Archive &ar) {
      ar & fee & meta;
    }

    nlohmann::json toJson() const;
  };

  /** Optional client-provided idempotency + validation window fields for user-submitted tx types. */
  struct TxIdempotencyWindow {
    uint64_t idempotentId{ 0 };    // Client-chosen unique id to prevent duplicates; 0 = disabled
    int64_t validationTsMin{ 0 };  // Validation window start (Unix seconds)
    int64_t validationTsMax{ 0 };  // Validation window end (Unix seconds)

    template <typename Archive> void serialize(Archive &ar) {
      ar & idempotentId & validationTsMin & validationTsMax;
    }

    nlohmann::json toJson() const;
  };

  struct TxDefault : TxCommon, TxIdempotencyWindow {
    uint64_t tokenId{ 0 };      // Token ID (0 = native token)
    uint64_t fromWalletId{ 0 }; // Source wallet ID
    uint64_t toWalletId{ 0 };   // Destination wallet ID
    uint64_t amount{ 0 };       // Transfer amount

    template <typename Archive> void serialize(Archive &ar) {
      ar & tokenId & fromWalletId & toWalletId & amount & fee & meta &
          idempotentId & validationTsMin & validationTsMax;
    }

    nlohmann::json toJson() const;
  };
  struct TxGenesis : TxCommon {
    template <typename Archive> void serialize(Archive &ar) {
      ar & fee & meta;
    }

    nlohmann::json toJson() const;
  };
  struct TxNewUser : TxCommon, TxIdempotencyWindow {
    uint64_t fromWalletId{ 0 }; // Source wallet ID
    uint64_t toWalletId{ 0 };   // Destination wallet ID
    uint64_t amount{ 0 };       // Initial funding amount (in genesis/native token)

    template <typename Archive> void serialize(Archive &ar) {
      ar & fromWalletId & toWalletId & amount & fee & meta &
          idempotentId & validationTsMin & validationTsMax;
    }

    nlohmann::json toJson() const;
  };
  struct TxConfig : TxCommon, TxIdempotencyWindow {
    template <typename Archive> void serialize(Archive &ar) {
      ar & fee & meta & idempotentId & validationTsMin & validationTsMax;
    }

    nlohmann::json toJson() const;
  };
  struct TxUserUpdate : TxCommon, TxIdempotencyWindow {
    uint64_t walletId{ 0 }; // Wallet ID being updated

    template <typename Archive> void serialize(Archive &ar) {
      ar & walletId & fee & meta & idempotentId & validationTsMin &
          validationTsMax;
    }

    nlohmann::json toJson() const;
  };
  struct TxRenewal : TxCommon {
    uint64_t walletId{ 0 }; // Wallet ID being renewed

    template <typename Archive> void serialize(Archive &ar) {
      ar & walletId & fee & meta;
    }

    nlohmann::json toJson() const;
  };
  struct TxEndUser : TxCommon {
    uint64_t walletId{ 0 }; // Wallet ID being ended

    template <typename Archive> void serialize(Archive &ar) {
      ar & walletId & fee & meta;
    }

    nlohmann::json toJson() const;
  };

  /** In-memory typed transaction payload for centralized dispatch. */
  using TypedTx = std::variant<TxDefault, TxGenesis, TxNewUser, TxConfig,
                               TxUserUpdate, TxRenewal, TxEndUser>;

  struct Error : RoeErrorBase {
    using RoeErrorBase::RoeErrorBase;
  };

  template <typename T> using Roe = ResultOrError<T, Error>;

  struct Record {
    uint16_t type{T_DEFAULT};
    std::string data; // Packed typed transaction payload (binaryPack(TxX))
    std::vector<std::string> signatures;

    template <typename Archive> void serialize(Archive &ar) {
      ar & type & data & signatures;
    }

    /** Decode this Record's packed payload into TypedTx. */
    Roe<TypedTx> decode() const;

    nlohmann::json toJson() const;
  };

  /**
   * Block data structure (without hash)
   */
  struct Block {
    static constexpr uint16_t CURRENT_VERSION = 1;

    uint64_t index{ 0 };
    int64_t timestamp{ 0 };
    std::vector<Record> records;
    std::string previousHash;
    uint64_t nonce{ 0 };
    uint64_t slot{ 0 };
    uint64_t slotLeader{ 0 };
    /** Cumulative count of transactions in all previous blocks (block 0 has 0). */
    uint64_t txIndex{ 0 };

    template <typename Archive> void serialize(Archive &ar) {
      ar & index & timestamp & records & previousHash & nonce & slot & slotLeader & txIndex;
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

  Ledger();
  ~Ledger() override = default;

  struct InitConfig {
    std::string workDir;
    uint64_t startingBlockId{ 0 };
  };

  uint64_t getNextBlockId() const;
  /** First valid block ID (same as startingBlockId from init/mount). No blocks when getNextBlockId() <= getStartingBlockId(). */
  uint64_t getStartingBlockId() const;

  Roe<void> init(const InitConfig& config);
  Roe<void> mount(const std::string& workDir);
  Roe<void> addBlock(const ChainNode& block);
  Roe<void> updateCheckpoints(const std::vector<uint64_t>& blockIds);
  Roe<ChainNode> readBlock(uint64_t blockId) const;
  Roe<ChainNode> readLastBlock() const;
  /** Smallest blockId such that block.timestamp >= timestamp (O(log n) block reads). */
  Roe<ChainNode> findBlockByTimestamp(int64_t timestamp) const;
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

  /** Cached latest block for fast readLastBlock/readBlock(lastId) access. */
  mutable std::optional<ChainNode> latestBlockCache_;

  bool loadIndex();
  bool saveIndex();
  Roe<void> cleanupData();
};

} // namespace pp