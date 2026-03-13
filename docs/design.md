# Time Chain

## 1. Chain: Slots, Blocks, and Epochs

Time is divided into fixed-duration **slots** (default 5 s). Every `slotsPerEpoch` slots (default 432 = 36 min) form an **epoch**. Each slot has at most one block; empty slots produce no block.

```mermaid
%%{init: {"theme": "base"}}%%
block-beta
  columns 12
  space:1
  block:epoch0["Epoch 0"]:6
    columns 6
    b0["Block 0\nslot 0"] b1["Block 1\nslot 1"] e0["…"] b431["Block N\nslot 431"]
  end
  block:epoch1["Epoch 1"]:6
    columns 6
    b432["Block\nslot 432"] e1["…(empty\nslot)"] e2["…"] b863["Block\nslot 863"]
  end
```

**Key relationships**

| Concept | Detail |
|---------|--------|
| Slot | Smallest time unit (`slotDuration` seconds). At most one block per slot. |
| Epoch | `slotsPerEpoch` consecutive slots. Slot leaders are chosen per epoch using VRF + stake. |
| Block | Carries a slot number, previous-block hash, slot-leader wallet ID, and a list of signed transactions. |

---

## 2. Block Structure

```mermaid
classDiagram
    class Block {
        uint64  index
        uint64  slot
        int64   timestamp
        uint64  slotLeader
        string  previousHash
        uint64  nonce
        uint64  txIndex
        SignedTransaction[] signedTxes
    }

    class SignedTransaction {
        Transaction tx
        string[]    signatures
    }

    class Transaction {
        uint16  type
        uint64  tokenId
        uint64  fromWalletId
        uint64  toWalletId
        uint64  amount
        uint64  fee
        string  meta
        uint64  idempotentId
        int64   validationTsMin
        int64   validationTsMax
    }

    Block "1" --> "0..*" SignedTransaction : contains
    SignedTransaction "1" --> "1" Transaction : wraps
```

**Transaction types**

| Type | Constant | Purpose |
|------|----------|---------|
| 1 | `T_GENESIS` | Initialize the system (genesis block) |
| 2 | `T_NEW_USER` | Fund / register a new user account |
| 3 | `T_CONFIG` | Update blockchain config |
| 4 | `T_USER` | User updates their own account info |
| 5 | `T_RENEWAL` | Miner renews an account (keeps it active) |
| 6 | `T_END_USER` | Miner terminates account (insufficient fee) |

---

## 3. Beacon, Relay, and Miners

There is exactly **one beacon** per network. Miners connect through one or more **relays**; relays forward to the beacon. The beacon never talks directly to untrusted miners.

```mermaid
flowchart TD
    B(["🔵 Beacon\n(single, port 8517)"])

    subgraph Relays
        R1["Relay 1\n(port 8519)"]
        R2["Relay 2\n(port 8520)"]
    end

    subgraph Miners
        M1["Miner A\n(port 8518)"]
        M2["Miner B"]
        M3["Miner C"]
        M4["Miner D"]
    end

    R1 -- sync blocks --> B
    R2 -- sync blocks --> B
    M1 -- beacon-compatible API --> R1
    M2 -- beacon-compatible API --> R1
    M3 -- beacon-compatible API --> R2
    M4 -- beacon-compatible API --> R2
```

**Roles**

| Node | Produces blocks | Stores full chain | Exposed to untrusted miners |
|------|:-:|:-:|:-:|
| Beacon | ✗ | ✓ | ✗ |
| Relay | ✗ | ✓ | ✓ |
| Miner | ✓ | partial | ✓ |

---

## 4. Checkpoints

A checkpoint is created when the stored chain data exceeds `minBlocks` blocks and those blocks are older than `minAgeSeconds`. Miners may start syncing from the **second-to-last checkpoint** (i.e. `checkpoint.lastId`) rather than from genesis, avoiding the need to replay the full history.

```mermaid
timeline
    title Checkpoint lifecycle (block IDs not to scale)
    section Genesis
        Block 0 : genesis block
    section Growing chain
        Block N : normal production
    section Checkpoint 1 (lastId = 0, currentId = N)
        Block N : criteria met – snapshot written
    section More blocks
        Block M : normal production
    section Checkpoint 2 (lastId = N, currentId = M)
        Block M : new snapshot
    section New miner joins
        Syncs from block N : second-to-last checkpoint (lastId of latest checkpoint)
```

**Checkpoint struct**

| Field | Meaning |
|-------|---------|
| `lastId` | Block ID of the previous checkpoint (miner sync start) |
| `currentId` | Block ID of this checkpoint |

---

## 5. System Reserved Wallets

Wallet IDs below `ID_FIRST_USER` (`1 << 20 = 1,048,576`) are **system reserved**. They are allowed to carry negative balances for accounting purposes and can issue tokens.

```mermaid
block-beta
  columns 1
  block:system["System-reserved range  (ID < 1,048,576)"]:1
    columns 4
    id0["ID 0\nGenesis\n(native token issuer)"]
    id1["ID 1\nFee collector"]
    id2["ID 2\nReserve"]
    id3["ID 3\nRecycle"]
    space:4
    gap["IDs 4 … 1,048,575\n(available for additional\nsystem tokens / roles)"]
  end
```

**Token partitioning** – each system wallet that acts as a token issuer can be dedicated to a token class:

```mermaid
mindmap
  root((System\nWallets))
    Native Token
      ID 0 genesis token
    Currency Tokens
      e.g. USD stablecoin
      e.g. EUR stablecoin
    Stock Tokens
      e.g. company equity
    Bond Tokens
      e.g. government bond
    RWA Tokens
      e.g. real-estate token
    Other Custom
      any further partition
```

Tokens are distinguished at runtime by `tokenId` (the issuing wallet ID). A transaction with `tokenId = 0` uses the native genesis token; any other value references a custom token issued by that system wallet.

---

## 6. User Wallets

User wallet IDs start at `ID_FIRST_USER` (`1,048,576`). Each account is an `AccountBuffer::Account` carrying a `Client::Wallet`.

```mermaid
classDiagram
    class UserAccount {
        uint64  id  ≥ ID_FIRST_USER
        Wallet  wallet
        string  meta
    }

    class Wallet {
        map~tokenId, balance~ mBalances
        string[]   publicKeys
        uint8      minSignatures
        uint8      keyType
    }

    class MetaField["meta (extensible)"] {
        -- today --
        arbitrary JSON / binary
        -- planned --
        NFTs
        Smart contracts
        Custom data schemas
    }

    UserAccount "1" --> "1" Wallet : owns
    UserAccount "1" --> "1" MetaField : carries
```

**Capabilities**

| Feature | Status |
|---------|--------|
| Multi-token balances | ✓ implemented |
| Multi-key / threshold signatures | ✓ implemented |
| Arbitrary `meta` field | ✓ implemented |
| NFT ownership records | planned |
| Smart contract storage | planned |
| Custom data schemas | planned |

---

## 7. Implementation Status

### Core Protocol

| Area | Component | Status |
|------|-----------|--------|
| Consensus | Ouroboros slot/epoch timing | ✅ done |
| Consensus | VRF-based slot-leader selection | ✅ done |
| Consensus | Stake-weighted election | ✅ done |
| Ledger | Block production & validation | ✅ done |
| Ledger | Persistent storage (FileStore / DirStore) | ✅ done |
| Ledger | Multi-token balances | ✅ done |
| Ledger | Transaction fee calculation (quadratic) | ✅ done |
| Checkpoints | Create / detect / reinitialize from checkpoint | ✅ done |

### Nodes

| Node | Feature | Status |
|------|---------|--------|
| Beacon | Full chain archival | ✅ done |
| Beacon | Checkpoint management | ✅ done |
| Relay | Sync from beacon, expose beacon API | ✅ done |
| Relay | DHT participation | ✅ done |
| Miner | Block production loop | ✅ done |
| Miner | Transaction pool | ✅ done |
| Miner | Reinit from checkpoint | ✅ done |

### Accounts & Tokens

| Feature | Status |
|---------|--------|
| System reserved wallets (IDs 0–3) | ✅ done |
| Native genesis token | ✅ done |
| Custom token issuance (by system wallets) | ✅ done |
| User wallet registration (`T_NEW_USER`) | ✅ done |
| Account renewal / termination | ✅ done |
| NFT support | ⬜ not started |
| Smart contracts | ⬜ not started |
| Currency / stock / bond / RWA token partitioning | ⬜ convention only |

### Interfaces & Tooling

| Feature | Status |
|---------|--------|
| TCP client/server (FetchClient/FetchServer) | ✅ done |
| CLI (`pp-client`) | ✅ done |
| HTTP API proxy (`pp-http`) | ✅ done |
| Node.js native addon | ✅ done |
| WebSocket / streaming API | ⬜ not started |
| Explorer UI | ⬜ not started |
