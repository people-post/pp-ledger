# Account recovery — design

**Status:** design (ar0). Phases: [PHASES.md](PHASES.md) · decisions: [DECISIONS.md](DECISIONS.md) ·
status: [CURRENT_STATE.md](CURRENT_STATE.md).

Today an account whose keys are all lost is lost: nothing can sign for it again. The genesis account
has a way back — its recovery set names a new admin set, applied after a delay
([BEACON_AUTHORITY § Recovery](../../docs/contracts/BEACON_AUTHORITY.md)). This project gives **every
user and issued account** the same way back, opt-in: a **recovery set** the owner chooses names the
account's new keys, and they take over after a **delay** during which **the owner can cancel** (AR002).

## What it protects against

| Case | Without | With recovery |
|------|---------|---------------|
| All the account's keys lost (a dead disk, a forgotten passphrase) | The account is lost | The recovery set names new keys; they take over after the delay |
| The recovery set colludes, or its keys are stolen | — | The owner sees the announcement (the wallet shows it, proven) and cancels it in the delay |
| One of several keys lost | The others still sign (multi-signature) | Same |
| A key stolen | The thief can spend at once | Same: recovery does not stop theft — evacuating does (pp-ledger-apps' wallet) |

## The rules

### The recovery set (AR003)

Part of the account's wallet, as its keys are — so in the account's leaf, proven like its balances:
`Wallet.recovery`:

| Field | Holds |
|-------|-------|
| `publicKeys`, `minSignatures` | Who may announce a recovery, and how many must sign (none: the account has no recovery) |
| `delayEpochs` | How long an announcement waits before it applies (at least 1) |
| `pending` | An announcement waiting: `publicKeys`, `minSignatures` (the account's next keys), `applyEpoch`, `announcedAt` (its block) |

The owner sets the set and the delay with their own update (`T_USER_UPDATE`, signed by the keys in
force), as they replace their keys. Only user and issued accounts have one (ids from 2²⁰): system
accounts are the genesis recovery set's to recover.

### Announce: `T_ACCOUNT_RECOVERY` (AR004)

A new record type (21): `walletId`, the announcement (`meta`: the next `publicKeys` and
`minSignatures`), `fee`, the validity window and idempotent id; **signed by the account's recovery set**
(its threshold), not by the account's keys. Its fee is the base fee: keys are not custom metadata (an
update's keys are not billed either). It is refused when the account has no recovery set, or one announcement is pending
already. The fee comes from the account's native balance (as every fee of the account does). Applying
it records `pending`, with `applyEpoch` = the block's epoch + `delayEpochs`.

### Cancel: the owner's update (AR002)

**Any `T_USER_UPDATE` of the account clears a pending announcement** — signed by the keys in force, it
proves the owner still holds them, so no recovery is needed. Cancelling is an update that changes
nothing else (pp-ledger-apps' wallet: *Cancel the recovery*). The account is **not frozen** while an
announcement waits: it spends, and its keys sign, as before.

### Apply (AR005)

At the first block of an epoch at or past `applyEpoch`, **after its records** (as miner liveness),
every node replaces the account's keys with the announced ones and clears `pending` — in place, as
miner state changes accounts (the account's `blockId`, where renewal reads its record, does not move).
After the records: a renewal in that block was built from the keys before, and an owner's update in it
cancels the announcement before it applies. The accounts are looked through once an epoch, at its
first block (or the first a node holds).

### Carried unchanged (AR006)

Like miner state, `Wallet.recovery` is chain-owned but for the owner's update setting the set: a
renewal carries it as it is; an update may set `publicKeys`, `minSignatures` and `delayEpochs`, and its
`pending` must be empty (it clears it). Account creation starts it empty (a `T_NEW_USER` may name a
set, never a pending announcement).

## Wire

- `Wallet` gains `recovery` (`AccountRecovery`): account records change their form — a new genesis
  (no network is live: AR007).
- Record type 21 `T_ACCOUNT_RECOVERY`; operation-file kind `account-recovery` (signed by the recovery
  set: `signers` names the recovery keys and their threshold, with `account` the recovered account).
- `pp-client`: `op update-keys` gains `--recovery-key` (repeat), `--recovery-min-signatures`,
  `--recovery-delay-epochs`; `op recover --account A --new-key K… --new-min-signatures N -f FEE`.

## In pp-ledger-apps

- **Wallet:** an account's *Recovery*: set or change its recovery set (keys from key cards or the vault —
  a backup key kept offline, friends' keys) and the delay (in days, as epochs); a pending announcement
  shown on Accounts, proven, with *Cancel the recovery*; *Recover an account* for recovery-key holders.
- **Token app:** the same for an issuer.

## Phases

[PHASES.md](PHASES.md): the state and rules (ar1), operation files and pp-client (ar2), the wallet and
token app (ar3), docs (ar4).
