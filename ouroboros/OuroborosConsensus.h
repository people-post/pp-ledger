#pragma once

#include "Module.h"
#include "IBlock.h"
#include "IBlockChain.h"
#include "ResultOrError.hpp"
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace pp {
namespace ouroboros {

struct StakeholderInfo {
    std::string id;
    uint64_t stake;
    
    StakeholderInfo(const std::string& stakeholderId, uint64_t stakeAmount)
        : id(stakeholderId), stake(stakeAmount) {}
};

/**
 * Ouroboros Consensus Protocol Implementation
 * 
 * Implements the Ouroboros Proof-of-Stake consensus algorithm.
 * Key features:
 * - Slot-based block production
 * - Epoch management
 * - Stake-based slot leader selection
 * - Chain selection rules
 */
class OuroborosConsensus : public Module {
public:
    /**
     * Constructor
     * @param slotDuration Duration of each slot in seconds
     * @param slotsPerEpoch Number of slots in each epoch
     */
    explicit OuroborosConsensus(uint64_t slotDuration = 1, uint64_t slotsPerEpoch = 21600);
    
    ~OuroborosConsensus() override = default;
    
    // Stakeholder management
    void registerStakeholder(const std::string& id, uint64_t stake);
    void updateStake(const std::string& id, uint64_t newStake);
    bool removeStakeholder(const std::string& id);
    
    // Epoch and slot management
    uint64_t getCurrentSlot() const;
    uint64_t getCurrentEpoch() const;
    uint64_t getSlotInEpoch(uint64_t slot) const;
    int64_t getSlotStartTime(uint64_t slot) const;
    
    // Slot leader selection
    ResultOrError<std::string, RoeErrorBase> getSlotLeader(uint64_t slot) const;
    bool isSlotLeader(uint64_t slot, const std::string& stakeholderId) const;
    
    // Block validation
    ResultOrError<bool, RoeErrorBase> validateBlock(
        const IBlock& block,
        const IBlockChain& chain) const;
    
    // Chain selection
    ResultOrError<bool, RoeErrorBase> shouldSwitchChain(
        const IBlockChain& currentChain,
        const IBlockChain& candidateChain) const;
    
    // Configuration
    void setSlotDuration(uint64_t seconds);
    void setSlotsPerEpoch(uint64_t slots);
    uint64_t getSlotDuration() const { return slotDuration_; }
    uint64_t getSlotsPerEpoch() const { return slotsPerEpoch_; }
    
    // Genesis time
    void setGenesisTime(int64_t timestamp);
    int64_t getGenesisTime() const { return genesisTime_; }
    
    // Utilities
    uint64_t getTotalStake() const;
    size_t getStakeholderCount() const;
    std::vector<StakeholderInfo> getStakeholders() const;
    
private:
    // Helper methods for slot leader selection
    std::string selectSlotLeader(uint64_t slot, uint64_t epoch) const;
    uint64_t calculateStakeThreshold(const std::string& stakeholderId, uint64_t totalStake) const;
    std::string hashSlotAndEpoch(uint64_t slot, uint64_t epoch) const;
    
    // Validation helpers
    bool validateSlotLeader(const IBlock& block, uint64_t slot) const;
    bool validateBlockTiming(const IBlock& block) const;
    bool validateChainDensity(const IBlockChain& chain, uint64_t fromSlot, uint64_t toSlot) const;
    
    // Data members
    std::map<std::string, uint64_t> stakeholders_;
    uint64_t slotDuration_;      // Duration of each slot in seconds
    uint64_t slotsPerEpoch_;     // Number of slots per epoch
    int64_t genesisTime_;        // Timestamp of genesis block
};

} // namespace ouroboros
} // namespace pp
