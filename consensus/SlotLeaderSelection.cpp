#include "SlotLeaderSelection.h"
#include <algorithm>
#include <iomanip>
#include <sstream>

namespace pp {
namespace consensus {

VRF::VRF() {
  setLogger("Vrf");
  log().info << "VRF module initialized";
}

VRF::Roe<VRF::VRFOutput> VRF::evaluate(const std::string &seed, uint64_t slot,
                                       const std::string &privateKey) const {

  if (privateKey.empty()) {
    return VRF::Error(1, "Private key cannot be empty");
  }

  // In a real implementation, this would use elliptic curve cryptography
  // For demonstration, we use a deterministic hash
  std::string input = hashInput(seed, slot, privateKey);

  // Generate VRF output
  std::string value = input;

  // Generate proof (simplified - in real VRF, this proves knowledge of private
  // key)
  std::stringstream proofStream;
  proofStream << "proof:" << input << ":slot:" << slot;
  std::string proof = proofStream.str();

  return VRFOutput(value, proof);
}

VRF::Roe<bool> VRF::verify(const std::string &output, const std::string &proof,
                           const std::string &seed, uint64_t slot,
                           const std::string &publicKey) const {

  if (publicKey.empty()) {
    return VRF::Error(2, "Public key cannot be empty");
  }

  // In real implementation, verify cryptographic proof
  // For demonstration, we do simple check

  std::string expectedInput = hashInput(seed, slot, publicKey);

  // Check if proof contains expected elements
  std::stringstream expectedProof;
  expectedProof << "proof:" << expectedInput << ":slot:" << slot;

  bool valid = (proof.find("proof:") == 0) &&
               (proof.find(std::to_string(slot)) != std::string::npos);

  return valid;
}

bool VRF::checkLeadership(const std::string &vrfOutput, uint64_t stake,
                          uint64_t totalStake, double difficulty) const {

  if (totalStake == 0 || stake == 0) {
    return false;
  }

  // Convert VRF output to number
  uint64_t outputNum = outputToNumber(vrfOutput);

  // Calculate threshold based on stake ratio
  // Higher stake = higher probability of winning
  double stakeRatio =
      static_cast<double>(stake) / static_cast<double>(totalStake);
  uint64_t threshold =
      static_cast<uint64_t>(UINT64_MAX * stakeRatio * difficulty);

  bool isLeader = outputNum < threshold;
  return isLeader;
}

std::string VRF::hashInput(const std::string &seed, uint64_t slot,
                           const std::string &key) const {
  // Simple hash function for demonstration
  std::stringstream ss;
  ss << seed << ":" << slot << ":" << key;
  std::string input = ss.str();

  uint64_t hash = 0xCBF29CE484222325ULL; // FNV offset basis
  for (char c : input) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 0x100000001B3ULL; // FNV prime
  }

  ss.str("");
  ss << std::hex << std::setfill('0') << std::setw(16) << hash;
  return ss.str();
}

uint64_t VRF::outputToNumber(const std::string &output) const {
  uint64_t result = 0;

  // Convert hex string to number
  for (size_t i = 0; i < std::min(output.size(), size_t(16)); ++i) {
    char c = output[i];
    uint64_t digit = 0;

    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = 10 + (c - 'a');
    } else if (c >= 'A' && c <= 'F') {
      digit = 10 + (c - 'A');
    }

    result = (result << 4) | digit;
  }

  return result;
}

// ========== EpochNonce Implementation ==========

EpochNonce::EpochNonce() {
  setLogger("EpochNonce");
  log().info << "Epoch nonce module initialized";
}

std::string
EpochNonce::generate(uint64_t epochNumber, const std::string &previousNonce,
                     const std::vector<std::string> &blockHashes) const {

  // Combine previous nonce with block hashes from the epoch
  std::stringstream ss;
  ss << "epoch:" << epochNumber << ":prev:" << previousNonce;

  if (!blockHashes.empty()) {
    std::string combined = combineHashes(blockHashes);
    ss << ":blocks:" << combined;
  }

  std::string input = ss.str();

  // Hash the combined input
  uint64_t hash = 0x811C9DC5; // FNV-1a 32-bit offset
  for (char c : input) {
    hash ^= static_cast<uint64_t>(c);
    hash *= 0x01000193; // FNV-1a 32-bit prime
  }

  ss.str("");
  ss << "nonce_" << std::hex << std::setfill('0') << std::setw(16) << hash;

  std::string nonce = ss.str();
  return nonce;
}

std::string EpochNonce::getGenesisNonce() const {
  return "genesis_nonce_0x0000000000000000";
}

std::string
EpochNonce::combineHashes(const std::vector<std::string> &hashes) const {
  if (hashes.empty()) {
    return "";
  }

  // XOR all hashes together
  std::stringstream result;

  for (const auto &hash : hashes) {
    result << hash;
  }

  std::string combined = result.str();

  // Take first 32 characters as representative hash
  if (combined.size() > 32) {
    combined = combined.substr(0, 32);
  }

  return combined;
}

} // namespace consensus
} // namespace pp
