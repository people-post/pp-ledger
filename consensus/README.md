# Consensus Library

This directory contains the implementation of the Ouroboros Proof-of-Stake consensus protocol for the pp-ledger blockchain.

## Overview

Ouroboros is a provably secure proof-of-stake blockchain protocol. This implementation provides the core consensus mechanisms needed for a stake-based blockchain system.

## Architecture

### Core Components

#### 1. Interfaces (`../interface/IBlock.hpp`, `../interface/IBlockChain.hpp`)
- **IBlock**: Interface for block data structures
  - Defines standard block properties (index, timestamp, hash, etc.)
  - Adds Ouroboros-specific fields (slot, slot leader)
  - Independent of server implementation

- **IBlockChain**: Interface for blockchain data structures
  - Defines chain operations (add, get, validate)
  - Query operations for blocks
  - Validation methods

#### 2. Consensus Core (`Ouroboros.h/cpp`)
- **Ouroboros**: Main consensus protocol implementation
  - Stakeholder registration and management
  - Slot and epoch time management
  - Slot leader selection
  - Block validation
  - Chain selection rules

**Key Features:**
- Slot-based block production
- Epoch management (configurable slots per epoch)
- Stake-weighted leader selection
- Chain density validation
- Longest valid chain selection

#### 3. Slot Leader Selection (`SlotLeaderSelection.h/cpp`)
- **VRF (Verifiable Random Function)**
  - Cryptographic proof generation for slot leadership
  - Proof verification
  - Leadership threshold calculation based on stake

- **EpochNonce**
  - Randomness generation for each epoch
  - Combines previous nonce with block hashes
  - Ensures unpredictability in leader selection

#### 4. Epoch & Slot Management (`EpochManager.h/cpp`)
- **EpochManager**
  - Epoch initialization and finalization
  - Slot-to-epoch mapping
  - Slot leader tracking per epoch
  - Time calculations for slots and epochs

- **SlotTimer**
  - Slot timing utilities
  - Current slot calculation
  - Time-in-slot validation
  - Countdown to next slot

## Key Concepts

### Slots
- Basic time unit in Ouroboros
- Fixed duration (default: 1 second, configurable)
- Each slot has at most one block
- Assigned to specific stakeholders

### Epochs
- Collection of consecutive slots
- Default: 21,600 slots per epoch (6 hours with 1s slots)
- Slot leaders determined at epoch start
- Provides long-term randomness stability

### Stake-Based Selection
- Stakeholders register with stake amount
- Higher stake = higher probability of being selected as slot leader
- Selection is deterministic but unpredictable
- Uses VRF for verifiable randomness

## Usage Example

```cpp
#include "Ouroboros.h"
#include "EpochManager.h"
#include "SlotLeaderSelection.h"

using namespace pp::consensus;

// Initialize consensus
Ouroboros consensus(1, 21600); // 1s slots, 21600 per epoch

// Register stakeholders
consensus.registerStakeholder("alice", 1000);
consensus.registerStakeholder("bob", 2000);
consensus.registerStakeholder("charlie", 500);

// Get current slot
uint64_t currentSlot = consensus.getCurrentSlot();
uint64_t currentEpoch = consensus.getCurrentEpoch();

// Determine slot leader
auto leaderResult = consensus.getSlotLeader(currentSlot);
if (leaderResult.isOk()) {
    std::string leader = leaderResult.value();
    // leader can now produce a block
}

// Validate a block
bool isLeader = consensus.isSlotLeader(currentSlot, "alice");

// VRF usage
VRF vrf;
auto vrfResult = vrf.evaluate("epoch_nonce", currentSlot, "alice_private_key");
if (vrfResult.isOk()) {
    auto output = vrfResult.value();
    bool wins = vrf.checkLeadership(output.value, 1000, 3500, 0.05);
}
```

## Testing

The library includes comprehensive tests in the `test/` directory:

- **test_ouroboros_consensus.cpp**: Tests for core consensus functionality
  - Stakeholder management (register, update, remove)
  - Slot and epoch calculations
  - Slot leader selection
  - Configuration updates
  - Error handling

- **test_ouroboros_epoch.cpp**: Tests for epoch and slot management
  - Epoch initialization and finalization
  - Slot time calculations
  - Epoch/slot conversions
  - Slot leader tracking
  - SlotTimer utilities

- **test_ouroboros_vrf.cpp**: Tests for VRF and randomness
  - VRF evaluation and verification
  - Leadership threshold checking
  - Epoch nonce generation
  - Determinism validation

Run tests:
```bash
cd build
make test_ouroboros_consensus test_ouroboros_epoch test_ouroboros_vrf
./test/test_ouroboros_consensus
./test/test_ouroboros_epoch
./test/test_ouroboros_vrf

# Or use CTest
ctest -R ouroboros
```

## Dependencies

This library uses the following from the `lib` directory:
- **Module**: Base class providing logging functionality
- **Logger**: Logging system for debugging and monitoring
- **ResultOrError**: Error handling with detailed error information

**No dependencies on `server` directory** - all blockchain data structures use interfaces.

## Integration with pp-ledger

To integrate with the main blockchain:

1. Implement `IBlock` and `IBlockChain` interfaces in your block/blockchain classes
2. Use `Ouroboros` for leader selection and validation
3. Use `EpochManager` to track epochs and slots
4. Use `VRF` for cryptographic proof of leadership

## Configuration Parameters

- **Slot Duration**: Time for each slot (default: 1 second)
- **Slots Per Epoch**: Number of slots in epoch (default: 21,600)
- **Genesis Time**: Starting timestamp for blockchain
- **VRF Difficulty**: Leadership probability multiplier (default: 0.05)

## Future Enhancements

- Implement true VRF using elliptic curve cryptography
- Add stake delegation mechanisms
- Implement reward distribution
- Add slashing for malicious behavior
- Optimize leader selection for large stakeholder sets
- Add chain density attack prevention
- Implement stake snapshots for epoch transitions

## References

- [Ouroboros: A Provably Secure Proof-of-Stake Blockchain Protocol](https://eprint.iacr.org/2016/889.pdf)
- [Ouroboros Praos](https://eprint.iacr.org/2017/573.pdf)
- [Cardano Documentation](https://docs.cardano.org/)

## License

See the main LICENSE file in the repository root.
