#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pp {

/**
 * Block data structure
 */
struct Block {
  static constexpr uint16_t CURRENT_VERSION = 1;

  Block();

  // Core block methods
  std::string calculateHash() const;

  // Long-term support serialization for disk persistence
  /**
   * Serialize block to binary format for long-term storage
   * Format is version-aware and compact for efficient disk storage
   * Binary format:
   * [version][index][timestamp][data_size+data][prevHash_size+prevHash][hash_size+hash][nonce][slot][leader_size+leader]
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
  uint64_t index;
  int64_t timestamp;
  std::string data;
  std::string previousHash;
  std::string hash;
  uint64_t nonce;
  uint64_t slot;
  std::string slotLeader;
};

} // namespace pp
