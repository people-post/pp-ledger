#ifndef PP_LEDGER_VALIDATOR_SERVER_H
#define PP_LEDGER_VALIDATOR_SERVER_H

#include "../ledger/Ledger.h"
#include "../lib/Service.h"
#include <nlohmann/json.hpp>
#include <string>

namespace pp {

/**
 * ValidatorServer - Base class for server implementations (MinerServer and BeaconServer)
 * 
 * Provides common functionality for:
 * - JSON serialization of blocks
 * - Shared server infrastructure
 */
class ValidatorServer : public Service {
public:
  ValidatorServer();
  virtual ~ValidatorServer() = default;

protected:
  // JSON serialization utilities (shared between MinerServer and BeaconServer)
  /**
   * Convert a ChainNode to JSON format for API responses
   * @param block The block to serialize
   * @return JSON object containing block data
   */
  nlohmann::json blockToJson(const Ledger::ChainNode& block) const;
  
  /**
   * Parse a JSON object to create a ChainNode
   * Note: hash is not calculated automatically - caller should use calculateHash()
   * @param blockJson JSON object containing block data
   * @return ChainNode parsed from JSON
   */
  Ledger::ChainNode jsonToBlock(const nlohmann::json& blockJson) const;
};

} // namespace pp

#endif // PP_LEDGER_VALIDATOR_SERVER_H
