#pragma once

#include "Module.h"
#include "IBlock.h"
#include <string>
#include <vector>
#include <map>
#include <cstdint>

namespace pp {
namespace consensus {

/**
 * Epoch Manager
 * 
 * Manages epoch transitions, slot assignments, and epoch-specific state.
 * In Ouroboros:
 * - Time is divided into epochs
 * - Each epoch contains a fixed number of slots
 * - Slot leaders are determined at the beginning of each epoch
 */
class EpochManager : public Module {
public:
    struct EpochInfo {
        uint64_t number;
        int64_t startTime;
        int64_t endTime;
        uint64_t startSlot;
        uint64_t endSlot;
        std::string nonce;
        std::map<uint64_t, std::string> slotLeaders;  // slot -> leader mapping
        
        EpochInfo() : number(0), startTime(0), endTime(0), 
                     startSlot(0), endSlot(0), nonce("") {}
    };
    
    /**
     * Constructor
     * @param slotsPerEpoch Number of slots in each epoch
     * @param slotDuration Duration of each slot in seconds
     */
    explicit EpochManager(uint64_t slotsPerEpoch = 21600, uint64_t slotDuration = 1);
    
    ~EpochManager() override = default;
    
    // Epoch operations
    void initializeEpoch(uint64_t epochNumber, const std::string& nonce);
    void finalizeEpoch(uint64_t epochNumber, const std::vector<std::string>& blockHashes);
    
    // Epoch queries
    EpochInfo getEpochInfo(uint64_t epochNumber) const;
    EpochInfo getCurrentEpochInfo() const;
    uint64_t getCurrentEpoch() const;
    bool isEpochInitialized(uint64_t epochNumber) const;
    
    // Slot leader management
    void setSlotLeader(uint64_t epochNumber, uint64_t slot, const std::string& leader);
    std::string getSlotLeader(uint64_t epochNumber, uint64_t slot) const;
    
    // Configuration
    void setGenesisTime(int64_t timestamp);
    int64_t getGenesisTime() const { return genesisTime_; }
    void setSlotsPerEpoch(uint64_t slots);
    uint64_t getSlotsPerEpoch() const { return slotsPerEpoch_; }
    void setSlotDuration(uint64_t duration);
    uint64_t getSlotDuration() const { return slotDuration_; }
    
    // Slot utilities
    uint64_t getCurrentSlot() const;
    uint64_t getEpochFromSlot(uint64_t slot) const;
    uint64_t getSlotInEpoch(uint64_t slot) const;
    int64_t getSlotStartTime(uint64_t slot) const;
    int64_t getSlotEndTime(uint64_t slot) const;
    
private:
    uint64_t slotsPerEpoch_;
    uint64_t slotDuration_;
    int64_t genesisTime_;
    std::map<uint64_t, EpochInfo> epochs_;
    mutable uint64_t cachedCurrentEpoch_;
    mutable int64_t lastUpdateTime_;
};

/**
 * Slot Timer
 * 
 * Manages timing and synchronization for slot-based consensus.
 * Provides utilities for:
 * - Waiting for specific slots
 * - Checking if current time is within a slot
 * - Calculating time until next slot
 */
class SlotTimer : public Module {
public:
    explicit SlotTimer(uint64_t slotDuration = 1);
    ~SlotTimer() override = default;
    
    /**
     * Get current slot number based on genesis time
     */
    uint64_t getCurrentSlot(int64_t genesisTime) const;
    
    /**
     * Get absolute time for start of a slot
     */
    int64_t getSlotStartTime(uint64_t slot, int64_t genesisTime) const;
    
    /**
     * Get absolute time for end of a slot
     */
    int64_t getSlotEndTime(uint64_t slot, int64_t genesisTime) const;
    
    /**
     * Check if given timestamp falls within a slot
     */
    bool isTimeInSlot(int64_t timestamp, uint64_t slot, int64_t genesisTime) const;
    
    /**
     * Calculate time remaining in current slot (in seconds)
     */
    int64_t getTimeUntilNextSlot(int64_t genesisTime) const;
    
    /**
     * Calculate time until a specific slot starts (in seconds)
     * Returns negative if slot has already started
     */
    int64_t getTimeUntilSlot(uint64_t slot, int64_t genesisTime) const;
    
    /**
     * Get current timestamp
     */
    int64_t getCurrentTime() const;
    
    // Configuration
    void setSlotDuration(uint64_t duration);
    uint64_t getSlotDuration() const { return slotDuration_; }
    
private:
    uint64_t slotDuration_;
};

} // namespace consensus
} // namespace pp
