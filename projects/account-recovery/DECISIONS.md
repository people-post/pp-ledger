# Account recovery — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/contracts/` and mark
the entry **superseded by** that doc. Built (ar1–ar2): each rule now in WIRE_SCHEMA § Account recovery. AR002 and AR006 were revised once built, before any release: the recovery set stands above the account's keys.

## AR001 — Every user and issued account may have a way back

**Date:** 2026-10-19
**Decision:** Opt-in, per account: a recovery set and a delay. User and issued accounts (ids from 2²⁰);
not system accounts, whose keys the genesis recovery set already covers.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** An account with every key lost is lost for good: the last-resort wallet cannot evacuate
what nothing signs for. Token issuers are organizations whose holders change; they need it as much.
**Alternatives:** Multi-signature only (2 of 3 with a backup key: works today, but every payment needs
two signatures).

## AR002 — The recovery set stands above the account's keys; an announcement freezes the account

**Date:** 2026-10-19 (revised the same day: first built as the owner's veto, no freeze)
**Decision:** The account's keys cannot cancel an announcement; only the recovery set withdraws it.
From the announcement until the new keys take over, the account is frozen: its keys sign nothing.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** Recovery must also work when the account's keys are stolen: with an owner's veto, the
thief cancels every recovery. The recovery set is the cold, more trusted authority (as genesis's is);
the account's keys are the hot ones, the likelier to leak. Without the freeze, a thief would have the
whole delay to empty the account. The cost: a recovery set that colludes, or whose keys are stolen,
can take the account — its threshold and its keys kept apart are the protection.
**Alternatives:** The owner's veto, never frozen (as first built: helps only with lost keys); a
recovery set above the keys but no freeze (the owner, or a thief, may move funds out in the delay).

## AR003 — The set lives in the wallet, proven with it

**Date:** 2026-10-19
**Decision:** `Wallet.recovery` (the set, the delay, any pending announcement), in the account's leaf.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** Under the state root, an announcement is proven like a balance: the owner's wallet shows
it without believing a node: an owner whose account is frozen learns why, and from whom.
**Alternatives:** In the account's attachment (not chain-owned; renewals carry attachments as opaque
bytes).

## AR004 — A record of its own, signed by the recovery set

**Date:** 2026-10-19
**Decision:** `T_ACCOUNT_RECOVERY` (21), signed by the account's recovery set: announce new keys,
withdraw that, or change the set (`AccountRecoveryAction`). Its fee from the account. One
announcement at a time; no change while one waits.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** The signers are not the account's keys, so no existing record fits; one record for the
set's three acts keeps one signature rule; the account pays as for anything done to it.
**Alternatives:** A `T_USER_UPDATE` the recovery set may sign (blurs who acts; the update rules would
grow a second signer set).

## AR005 — Applied at an epoch's first block, after its records

**Date:** 2026-10-19
**Decision:** Due announcements apply after the records of an epoch's first block (or the first a node
holds), in place; the accounts are looked through once an epoch.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** Deterministic on every node, as miner liveness. After the records, not before: a
renewal in that block is built from the keys before, and the set's withdrawal in it wins. Once
an epoch, an in-memory pass over the accounts is cheap; an index would have to follow every rollback
of the account state.
**Alternatives:** Before the records (as first designed: refuses that block's renewals); an index by
`applyEpoch`.

## AR006 — The account's keys name the first set; then only the set changes itself

**Date:** 2026-10-19 (revised the same day: first, the owner's update set it)
**Decision:** Account creation or the account's own update may name a recovery set while it has
none. After that, renewals and updates carry `Wallet.recovery` unchanged; only the set's `CHANGE`
replaces or removes it. If too many of the set's keys are lost to act, it stays as it is.
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** An authority above the keys that the keys could replace is no authority: a thief
would replace it first. The first naming by the keys is safe — whoever holds them controls an account
without a set already.
**Alternatives:** The account's keys propose a new set after a long delay the set may veto (a way out
when the set's keys are lost; left out until asked for).

## AR007 — No migration

**Date:** 2026-10-19
**Decision:** The account record changes form; networks start from a new genesis. None is live.
**Rationale:** The owner's call: nothing released runs on these records.

## AR008 — The delay ends at an epoch start, at least half an epoch on

**Date:** 2026-10-19
**Decision:** The delay is in seconds, at least half an epoch. An announcement takes over at the first
epoch that starts at least the delay after its slot starts (`recoveryApplyEpoch`).
**Superseded by:** [WIRE_SCHEMA.md § Account recovery](../../docs/contracts/WIRE_SCHEMA.md#account-recovery) (normative; this entry keeps the why).
**Rationale:** Counted in whole epochs from the announcing epoch, a one-epoch delay announced in an
epoch's last slot took over one slot later. In time from the slot, it never comes sooner than the
delay; at an epoch start, every node applies it at the same block. Slot length is fixed at genesis,
so seconds convert to slots exactly.
**Alternatives:** Whole epochs counted from the epoch after the announcing one (the same guarantee,
coarser).
