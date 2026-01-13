#include "Ouroboros.h"
#include <algorithm>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <numeric>

namespace pp {
namespace consensus {

Ouroboros::Ouroboros(uint64_t slotDuration, uint64_t slotsPerEpoch)
    : Module("consensus")
    , slotDuration_(slotDuration)
    , slotsPerEpoch_(slotsPerEpoch)
    , genesisTime_(0) {
    
    // Set genesis time to current time if not set
    auto now = std::chrono::system_clock::now();
    genesisTime_ = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    
    log().info << "Ouroboros consensus initialized with slot duration: " + 
                  std::to_string(slotDuration_) + "s, slots per epoch: " + 
                  std::to_string(slotsPerEpoch_);
}

void Ouroboros::registerStakeholder(const std::string& id, uint64_t stake) {
    if (stake == 0) {
        log().warning << "Cannot register stakeholder '" + id + "' with zero stake";
        return;
    }
    
    stakeholders_[id] = stake;
    log().info << "Registered stakeholder '" + id + "' with stake: " + std::to_string(stake);
}

void Ouroboros::updateStake(const std::string& id, uint64_t newStake) {
    auto it = stakeholders_.find(id);
    if (it == stakeholders_.end()) {
        log().warning << "Cannot update stake for unknown stakeholder: " + id;
        return;
    }
    
    uint64_t oldStake = it->second;
    it->second = newStake;
    log().info << "Updated stake for '" + id + "' from " + 
                  std::to_string(oldStake) + " to " + std::to_string(newStake);
}

bool Ouroboros::removeStakeholder(const std::string& id) {
    auto it = stakeholders_.find(id);
    if (it == stakeholders_.end()) {
        log().warning << "Cannot remove unknown stakeholder: " + id;
        return false;
    }
    
    stakeholders_.erase(it);
    log().info << "Removed stakeholder: " + id;
    return true;
}

uint64_t Ouroboros::getCurrentSlot() const {
    auto now = std::chrono::system_clock::now();
    int64_t currentTime = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    
    if (currentTime < genesisTime_) {
        return 0;
    }
    
    int64_t elapsed = currentTime - genesisTime_;
    return static_cast<uint64_t>(elapsed / slotDuration_);
}

uint64_t Ouroboros::getCurrentEpoch() const {
    uint64_t slot = getCurrentSlot();
    return slot / slotsPerEpoch_;
}

uint64_t Ouroboros::getSlotInEpoch(uint64_t slot) const {
    return slot % slotsPerEpoch_;
}

int64_t Ouroboros::getSlotStartTime(uint64_t slot) const {
    return genesisTime_ + static_cast<int64_t>(slot * slotDuration_);
}

Ouroboros::Roe<std::string> Ouroboros::getSlotLeader(uint64_t slot) const {
    if (stakeholders_.empty()) {
        return RoeErrorBase(1, "No stakeholders registered");
    }
    
    uint64_t epoch = slot / slotsPerEpoch_;
    std::string leader = selectSlotLeader(slot, epoch);
    
    return leader;
}

bool Ouroboros::isSlotLeader(uint64_t slot, const std::string& stakeholderId) const {
    auto result = getSlotLeader(slot);
    if (!result.isOk()) {
        return false;
    }
    
    return result.value() == stakeholderId;
}

std::string Ouroboros::selectSlotLeader(uint64_t slot, uint64_t epoch) const {
    // Simple deterministic leader selection based on stake weight
    // In production, this would use VRF (Verifiable Random Function)
    
    uint64_t totalStake = getTotalStake();
    if (totalStake == 0) {
        return "";
    }
    
    // Create a deterministic hash from slot and epoch
    std::string slotHash = hashSlotAndEpoch(slot, epoch);
    
    // Convert hash to a number in range [0, totalStake)
    uint64_t hashValue = 0;
    for (size_t i = 0; i < std::min(slotHash.size(), size_t(8)); ++i) {
        hashValue = (hashValue << 8) | static_cast<uint8_t>(slotHash[i]);
    }
    uint64_t position = hashValue % totalStake;
    
    // Select stakeholder based on cumulative stake
    uint64_t cumulative = 0;
    for (const auto& [id, stake] : stakeholders_) {
        cumulative += stake;
        if (position < cumulative) {
            return id;
        }
    }
    
    // Fallback to first stakeholder
    return stakeholders_.begin()->first;
}

std::string Ouroboros::hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const {
    // Simple hash function (in production, use cryptographic hash)
    std::stringstream ss;
    ss << "slot:" << slot << ":epoch:" << epoch;
    std::string input = ss.str();
    
    // Simple hash computation
    uint64_t hash = 0x123456789abcdef0ULL;
    for (char c : input) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 0x100000001b3ULL;
    }
    
    ss.str("");
    ss << std::hex << std::setfill('0') << std::setw(16) << hash;
    return ss.str();
}

uint64_t Ouroboros::getTotalStake() const {
    return std::accumulate(stakeholders_.begin(), stakeholders_.end(), uint64_t(0),
                          [](uint64_t sum, const auto& pair) {
                              return sum + pair.second;
                          });
}

size_t Ouroboros::getStakeholderCount() const {
    return stakeholders_.size();
}

std::vector<Ouroboros::StakeholderInfo> Ouroboros::getStakeholders() const {
    std::vector<Ouroboros::StakeholderInfo> result;
    result.reserve(stakeholders_.size());
    
    for (const auto& [id, stake] : stakeholders_) {
        result.emplace_back(id, stake);
    }
    
    return result;
}

Ouroboros::Roe<bool> Ouroboros::validateBlock(
    const IBlock& block,
    const IBlockChain& chain) const {
    
    uint64_t slot = block.getSlot();
    std::string slotLeader = block.getSlotLeader();
    
    // Validate slot leader
    if (!validateSlotLeader(block, slot)) {
        return RoeErrorBase(2, "Invalid slot leader for block at slot " + std::to_string(slot));
    }
    
    // Validate block timing
    if (!validateBlockTiming(block)) {
        return RoeErrorBase(3, "Block timestamp outside valid slot range");
    }
    
    // Validate hash chain
    if (chain.getSize() > 0) {
        auto latestBlock = chain.getLatestBlock();
        if (latestBlock && block.getPreviousHash() != latestBlock->getHash()) {
            return RoeErrorBase(4, "Block previous hash does not match chain");
        }
        
        if (block.getIndex() != latestBlock->getIndex() + 1) {
            return RoeErrorBase(5, "Block index mismatch");
        }
    }
    
    // Validate block hash
    std::string calculatedHash = block.calculateHash();
    if (calculatedHash != block.getHash()) {
        return RoeErrorBase(6, "Block hash validation failed");
    }
    
    return true;
}

bool Ouroboros::validateSlotLeader(const IBlock& block, uint64_t slot) const {
    std::string expectedLeader = selectSlotLeader(slot, slot / slotsPerEpoch_);
    return block.getSlotLeader() == expectedLeader;
}

bool Ouroboros::validateBlockTiming(const IBlock& block) const {
    uint64_t slot = block.getSlot();
    int64_t slotStart = getSlotStartTime(slot);
    int64_t slotEnd = slotStart + static_cast<int64_t>(slotDuration_);
    
    int64_t blockTime = block.getTimestamp();
    
    return blockTime >= slotStart && blockTime < slotEnd;
}

Ouroboros::Roe<bool> Ouroboros::shouldSwitchChain(
    const IBlockChain& currentChain,
    const IBlockChain& candidateChain) const {
    
    // Implement chain selection rule (longest valid chain)
    size_t currentSize = currentChain.getSize();
    size_t candidateSize = candidateChain.getSize();
    
    if (candidateSize <= currentSize) {
        return false;
    }
    
    // Validate candidate chain density (not too sparse)
    if (candidateSize > 0) {
        auto latestBlock = candidateChain.getLatestBlock();
        if (latestBlock) {
            uint64_t latestSlot = latestBlock->getSlot();
            if (!validateChainDensity(candidateChain, 0, latestSlot)) {
                return RoeErrorBase(7, "Candidate chain density too low");
            }
        }
    }
    
    return true;
}

bool Ouroboros::validateChainDensity(
    const IBlockChain& chain,
    uint64_t fromSlot,
    uint64_t toSlot) const {
    
    // Simple density check: at least 50% of slots should have blocks
    // In production, this would be more sophisticated
    
    if (toSlot <= fromSlot) {
        return true;
    }
    
    uint64_t slotRange = toSlot - fromSlot + 1;
    size_t blockCount = chain.getSize();
    
    double density = static_cast<double>(blockCount) / static_cast<double>(slotRange);
    
    return density >= 0.5;
}

void Ouroboros::setSlotDuration(uint64_t seconds) {
    slotDuration_ = seconds;
    log().info << "Slot duration updated to " + std::to_string(seconds) + " seconds";
}

void Ouroboros::setSlotsPerEpoch(uint64_t slots) {
    slotsPerEpoch_ = slots;
    log().info << "Slots per epoch updated to " + std::to_string(slots);
}

void Ouroboros::setGenesisTime(int64_t timestamp) {
    genesisTime_ = timestamp;
    log().info << "Genesis time set to " + std::to_string(timestamp);
}

} // namespace consensus
} // namespace pp
