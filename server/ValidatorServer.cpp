#include "ValidatorServer.h"
#include "../lib/Logger.h"

namespace pp {

ValidatorServer::ValidatorServer() {}

nlohmann::json ValidatorServer::blockToJson(const Ledger::ChainNode& block) const {
  nlohmann::json blockJson;
  blockJson["index"] = block.block.index;
  blockJson["timestamp"] = block.block.timestamp;
  blockJson["hash"] = block.hash;
  blockJson["previousHash"] = block.block.previousHash;
  blockJson["slot"] = block.block.slot;
  blockJson["slotLeader"] = block.block.slotLeader;
  
  // Serialize transactions
  blockJson["signedTxes"] = nlohmann::json::array();
  for (const auto& signedTx : block.block.signedTxes) {
    nlohmann::json txJson;
    txJson["type"] = signedTx.obj.type;
    txJson["fromWalletId"] = signedTx.obj.fromWalletId;
    txJson["toWalletId"] = signedTx.obj.toWalletId;
    txJson["amount"] = signedTx.obj.amount;
    txJson["meta"] = signedTx.obj.meta;
    txJson["signature"] = signedTx.signature;
    blockJson["signedTxes"].push_back(txJson);
  }
  
  return blockJson;
}

Ledger::ChainNode ValidatorServer::jsonToBlock(const nlohmann::json& blockJson) const {
  Ledger::ChainNode block;
  
  if (blockJson.contains("index")) block.block.index = blockJson["index"].get<uint64_t>();
  if (blockJson.contains("timestamp")) block.block.timestamp = blockJson["timestamp"].get<int64_t>();
  if (blockJson.contains("previousHash")) block.block.previousHash = blockJson["previousHash"].get<std::string>();
  if (blockJson.contains("slot")) block.block.slot = blockJson["slot"].get<uint64_t>();
  if (blockJson.contains("slotLeader")) block.block.slotLeader = blockJson["slotLeader"].get<std::string>();
  if (blockJson.contains("hash")) block.hash = blockJson["hash"].get<std::string>();
  
  // Parse signedTxes array
  if (blockJson.contains("signedTxes") && blockJson["signedTxes"].is_array()) {
    block.block.signedTxes.clear();
    for (const auto& txJson : blockJson["signedTxes"]) {
      Ledger::SignedData<Ledger::Transaction> signedTx;
      if (txJson.contains("type")) signedTx.obj.type = txJson["type"].get<uint16_t>();
      if (txJson.contains("fromWalletId")) signedTx.obj.fromWalletId = txJson["fromWalletId"].get<uint64_t>();
      if (txJson.contains("toWalletId")) signedTx.obj.toWalletId = txJson["toWalletId"].get<uint64_t>();
      if (txJson.contains("amount")) signedTx.obj.amount = txJson["amount"].get<int64_t>();
      if (txJson.contains("meta")) signedTx.obj.meta = txJson["meta"].get<std::string>();
      if (txJson.contains("signature")) signedTx.signature = txJson["signature"].get<std::string>();
      block.block.signedTxes.push_back(signedTx);
    }
  }
  
  return block;
}

} // namespace pp
