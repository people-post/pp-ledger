# Miner Architecture

## Overview

The Miner is responsible for block production in the pp-ledger blockchain system. It works alongside the Beacon (which handles validation and checkpointing) to maintain the distributed ledger.

## Key Responsibilities

1. **Block Production**: Create new blocks when selected as slot leader
2. **Transaction Management**: Maintain a pool of pending transactions
3. **Checkpoint Reinitialization**: Reinitialize from checkpoints when out of date
4. **Chain Synchronization**: Sync with other nodes when behind
5. **Consensus Participation**: Use Ouroboros PoS to determine slot leadership

## Architecture

### Core Components

```cpp
class Miner : public Module {
private:
    consensus::Ouroboros consensus_;  // Consensus protocol
    Ledger ledger_;                   // Persistent storage
    BlockChain chain_;                // In-memory chain
    std::queue<Ledger::Transaction> pendingTransactions_;  // Transaction pool
};
```

### Configuration

```cpp
struct Config {
    std::string minerId;              // Unique miner identifier
    uint64_t stake;                   // Miner's stake in the network
    std::string workDir;              // Directory for data storage
    uint32_t slotDuration;            // Duration of each slot (seconds)
    uint32_t slotsPerEpoch;           // Number of slots per epoch
    size_t maxPendingTransactions;    // Max size of transaction pool
    size_t maxTransactionsPerBlock;   // Max transactions per block
};
```

## Block Production Flow

1. **Check Leadership**: `isSlotLeader()` checks if miner is slot leader
2. **Create Block**: `produceBlock()` creates new block with pending transactions
3. **Validate**: Block is validated against consensus rules
4. **Persist**: Block is added to in-memory chain and persisted to ledger
5. **Broadcast**: Block should be broadcast to network (handled by MinerServer)

### Example Usage

```cpp
pp::Miner miner;

// Initialize
pp::Miner::Config config;
config.minerId = "miner1";
config.stake = 1000000;
config.workDir = "/data/miner1";
config.slotDuration = 1;
config.slotsPerEpoch = 21600;
config.maxPendingTransactions = 10000;
config.maxTransactionsPerBlock = 100;

auto initResult = miner.init(config);
if (!initResult) {
    // Handle error
}

// Add transactions to pool
Ledger::Transaction tx;
tx.fromWallet = "alice";
tx.toWallet = "bob";
tx.amount = 100;
miner.addTransaction(tx);

// Check if we should produce a block
if (miner.isSlotLeader()) {
    auto blockResult = miner.produceBlock();
    if (blockResult) {
        auto block = blockResult.value();
        // Broadcast block to network
    }
}
```

## Transaction Management

### Transaction Pool

- Maintains a FIFO queue of pending transactions
- Maximum size configurable via `maxPendingTransactions`
- Thread-safe access with mutex protection
- Transactions removed when included in blocks

### Transaction Selection

```cpp
std::vector<Ledger::Transaction> selectTransactionsForBlock();
```

- Selects up to `maxTransactionsPerBlock` transactions
- Uses FIFO ordering (simple strategy)
- Future enhancements: fee prioritization, anti-spam measures

## Checkpoint Reinitialization

### Problem

New miners or miners that have been offline may have outdated ledger data. Instead of syncing from genesis, they can reinitialize from a recent checkpoint.

### Solution

```cpp
Roe<void> reinitFromCheckpoint(const CheckpointInfo& checkpoint);
```

**Process:**
1. Load checkpoint state (balances, stakes, etc.)
2. Clear existing transaction pool
3. Rebuild ledger from checkpoint block ID
4. Prune old blocks before checkpoint
5. Resume normal operation from checkpoint

### When to Reinitialize

```cpp
bool isOutOfDate(uint64_t checkpointId) const;
```

Miner is considered out of date if:
- Remote checkpoint is > 1000 blocks ahead
- Local data is corrupted or inconsistent
- Explicit reinitialization requested

## Consensus Integration

### Ouroboros Proof-of-Stake

The Miner uses Ouroboros consensus for:

1. **Slot Leadership**: Determine when to produce blocks
2. **Block Validation**: Verify blocks from other miners
3. **Epoch Management**: Track epoch transitions
4. **Stake Management**: Register and update stake

### Slot-Based Production

```cpp
uint64_t currentSlot = miner.getCurrentSlot();
bool isLeader = miner.isSlotLeader(currentSlot);
```

- Time is divided into slots (e.g., 1 second each)
- Each slot has one designated leader
- Leader is selected based on stake proportionally
- Multiple epochs group slots together

## Validation

### Block Validation

```cpp
Roe<void> validateBlock(const Block& block) const;
```

**Checks:**
1. **Consensus Rules**: Block follows PoS protocol
2. **Sequence**: Block index is sequential
3. **Hash Chain**: Previous hash matches
4. **Slot Leader**: Leader is valid for that slot
5. **Timestamp**: Block time within slot boundaries

### Helper Validators

- `isValidBlockSequence()`: Check block follows previous
- `isValidSlotLeader()`: Verify leader eligibility
- `isValidTimestamp()`: Ensure timestamp in slot range

## Thread Safety

### Mutexes

- `stateMutex_`: Protects initialization and state changes
- `transactionMutex_`: Protects transaction pool access

### Safe Operations

All public methods are thread-safe:
- Adding transactions
- Producing blocks
- Checking leadership status
- Querying state

## Integration with Beacon

### Division of Responsibilities

**Beacon:**
- Validates blocks (doesn't produce)
- Creates and manages checkpoints
- Serves as source of truth
- Coordinates network decisions

**Miner:**
- Produces blocks (doesn't checkpoint)
- Manages transaction pool
- Reinitializes from checkpoints
- Contributes to chain growth

### Communication Flow

```
Miner -> Produces Block -> Broadcast to Network
                                |
                                v
                           Beacon -> Validates Block -> Stores in Ledger
                                |
                                v
                           Beacon -> Evaluates Checkpoint Criteria
                                |
                                v
                           Beacon -> Creates Checkpoint (if needed)
                                |
                                v
                           Miner <- Receives Checkpoint <- Broadcast
                                |
                                v
                           Miner -> Reinitializes if out of date
```

## Storage Management

### Work Directory Structure

```
workDir/
  ledger/
    blocks/
      00000/
      00001/
      ...
    index.dat
    checkpoints.dat
```

### Ledger Persistence

- Blocks stored in `DirDirStore` backend
- Automatic file rotation when files reach size limit
- Recursive directory structure for scalability
- Index maintained for fast block lookup

## Error Handling

### Error Codes

- `1`: Miner already initialized
- `2`: Failed to initialize ledger
- `3`: Failed to load checkpoint
- `4`: Failed to rebuild ledger
- `5`: Miner not initialized
- `6`: Not slot leader for current slot
- `7`: Failed to create block
- `8`: Failed to add block to chain
- `9`: Transaction pool full
- `10-17`: Block validation errors
- `18`: Block not found
- `19-21`: Checkpoint errors

### Error Recovery

- Transaction pool full: Wait and retry
- Not slot leader: Wait for next slot
- Block creation failed: Log and skip slot
- Ledger persistence failed: Continue with in-memory (log warning)

## Future Enhancements

### Transaction Pool Improvements

- **Fee-based prioritization**: Higher fees get included first
- **Anti-spam measures**: Reject obviously invalid transactions
- **Mempool eviction**: Remove old/stale transactions
- **Transaction validation**: Check signatures, balances before accepting

### Checkpoint Optimization

- **Incremental checkpoints**: Only store state changes
- **Compressed checkpoints**: Reduce storage/bandwidth
- **Checkpoint verification**: Verify checkpoint integrity before applying
- **Partial reinitialization**: Only sync missing blocks

### Performance

- **Parallel validation**: Validate blocks concurrently
- **Batch processing**: Process multiple transactions together
- **Cache optimization**: Cache frequently accessed data
- **Async I/O**: Non-blocking ledger operations

### Monitoring

- **Metrics**: Block production rate, transaction throughput
- **Health checks**: Monitor sync status, slot leadership
- **Alerting**: Notify on critical failures
- **Diagnostics**: Detailed logging for debugging

## Testing Considerations

### Unit Tests

- Block production logic
- Transaction pool management
- Checkpoint reinitialization
- Validation helpers
- Error handling

### Integration Tests

- Miner-Beacon interaction
- Multi-node synchronization
- Checkpoint creation and application
- Network partition recovery
- Long-running stability

### Performance Tests

- Maximum transaction throughput
- Block production latency
- Checkpoint loading time
- Memory usage under load
- Disk I/O patterns

## Security Considerations

1. **Stake Validation**: Verify miner has claimed stake before producing
2. **Timestamp Manipulation**: Prevent miners from manipulating block times
3. **Double Production**: Prevent producing multiple blocks for same slot
4. **Checkpoint Integrity**: Validate checkpoint signatures/hashes
5. **Transaction Validation**: Verify transactions before including in blocks

## References

- **Ouroboros Paper**: [Ouroboros: A Provably Secure Proof-of-Stake Blockchain Protocol](https://eprint.iacr.org/2016/889.pdf)
- **Consensus Module**: `/workspaces/pp-ledger/consensus/`
- **Ledger Module**: `/workspaces/pp-ledger/ledger/`
- **Beacon Architecture**: `/workspaces/pp-ledger/server/BEACON_ARCHITECTURE.md`
