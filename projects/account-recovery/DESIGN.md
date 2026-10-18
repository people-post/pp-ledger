# Account recovery — design

**Status:** built: the rules and pp-client here (ar1–ar2, normative in [WIRE_SCHEMA § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery)), the wallet and token app in pp-ledger-apps (ar3), docs (ar4). Phases: [PHASES.md](PHASES.md) · decisions: [DECISIONS.md](DECISIONS.md) ·
status: [CURRENT_STATE.md](CURRENT_STATE.md).

Today an account whose keys are all lost is lost, and one whose keys are stolen is the thief's. The
genesis account has a way back — its recovery set names a new admin set, applied after a delay
([BEACON_AUTHORITY § Recovery](../../docs/contracts/BEACON_AUTHORITY.md)). This project gives **every
user and issued account** the same, opt-in: a **recovery set** that **stands above the account's own
keys** (AR002) — cold keys, kept apart, that can give the account new keys, freezing it meanwhile.

## What it protects against

| Case | Without | With recovery |
|------|---------|---------------|
| All the account's keys lost (a dead disk, a forgotten passphrase) | The account is lost | The recovery set names new keys; they take over after the delay |
| A key stolen | The thief keeps the account | The recovery set announces new keys: the account is frozen at once, and the thief's key never signs again. What it sent before is gone |
| The recovery set colludes, or its keys are stolen | — | It can take the account: the protection is its threshold and keeping its keys cold and apart |
| One of several keys lost | The others still sign (multi-signature) | Same |

## The rules

### The recovery set (AR003)

Part of the account's wallet, as its keys are — so in the account's leaf, proven like its balances:
`Wallet.recovery`:

| Field | Holds |
|-------|-------|
| `publicKeys`, `minSignatures` | Who acts, and how many must sign (none: the account has no recovery) |
| `delaySeconds` | From an announcement to the earliest its keys take over: at least half an epoch (AR008) |
| `pending` | An announcement waiting: the account's next keys, `applyEpoch`, `announcedAt` (its block) |

**The account's keys name the first set** (`T_NEW_USER`, or their own `T_USER_UPDATE`). After that
**only the set changes itself** (AR006): the account's keys can neither change nor remove it. Only
user and issued accounts have one (ids from 2²⁰): system accounts are the genesis recovery set's.

### The set's actions: `T_ACCOUNT_RECOVERY` (AR004)

A record type (21), **signed by the account's recovery set** (its threshold), never by its keys; the
account pays the base fee (keys are not custom metadata). Its `meta` names one action:

- **Announce** the account's next keys: the account is **frozen** (AR002) until they take over.
- **Withdraw** an announcement: the account's keys sign again.
- **Change** the set (keys, threshold, delay), or remove it — not while an announcement waits.

### Frozen while announced (AR002)

From the announcement on, every record the account's own keys sign is refused — transfers out,
account creation from it, its updates, miner registration — in the pool and in blocks, by the state at
that point of the block. Transfers to it and its renewals go on. A thief holding the account's keys
is stopped from the announcement on; an owner is too, which is why the set is chosen with care.

### Apply (AR005, AR008)

An announcement takes over at the **first epoch that starts at least `delaySeconds` after its slot**
— never at the very next epoch start when the announcement came late in an epoch. At that epoch's
first block (or the first a node holds), **after its records** (as miner liveness), every node
replaces the account's keys with the announced ones and clears `pending` — in place, as miner state
changes accounts (the account's `blockId`, where renewal reads its record, does not move).

### Carried unchanged (AR006)

Like miner state, `Wallet.recovery` is chain-owned once set: a renewal or an update carries it as it
is; only the account's first naming of a set, and the set's own records, change it.

## Wire

- `Wallet` gains `recovery` (`AccountRecovery`): account records change their form — a new genesis
  (no network is live: AR007).
- Record type 21 `T_ACCOUNT_RECOVERY`; operation-file kind `account-recovery` (signed by the recovery
  set: `signers` names the recovery keys and their threshold, with `account` the recovered account).
- `pp-client`: `op set-recovery --account A --recovery-key K… --recovery-min-signatures N
  --delay-seconds S` (or `--none`: the account's keys name the first set, the set signs any change);
  `op recover --account A --new-key K… --new-min-signatures N`; `op withdraw-recovery --account A`
  (signers: A's recovery set, read from the node).

## In pp-ledger-apps

- **Wallet:** an account's *Recovery*: name its first recovery set (keys from key cards — a backup key
  kept in another wallet, friends' keys — and the delay in days); an announcement shown on Accounts,
  proven, with the account frozen; for whoever holds an account's recovery keys: announce its new keys
  (the owner's key card, or a new key made there), withdraw that, or change the set.
- **Token app:** an issuer names its first recovery set on Signers, and a change signed by the set;
  the announcement on Overview; its recovery-key holders act with the wallet.

## Phases

[PHASES.md](PHASES.md): the state and rules (ar1), operation files and pp-client (ar2), the wallet and
token app (ar3), docs (ar4).
