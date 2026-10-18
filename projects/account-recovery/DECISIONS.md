# Account recovery — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/contracts/` and mark
the entry **superseded by** that doc.

## AR001 — Every user and issued account may have a way back

**Date:** 2026-10-19
**Decision:** Opt-in, per account: a recovery set and a delay. User and issued accounts (ids from 2²⁰);
not system accounts, whose keys the genesis recovery set already covers.
**Rationale:** An account with every key lost is lost for good: the last-resort wallet cannot evacuate
what nothing signs for. Token issuers are organizations whose holders change; they need it as much.
**Alternatives:** Multi-signature only (2 of 3 with a backup key: works today, but every payment needs
two signatures).

## AR002 — The owner can cancel; the account is not frozen

**Date:** 2026-10-19
**Decision:** While an announcement waits, the account works as before, and any update signed by its
keys in force cancels it.
**Rationale:** The case is lost keys, not stolen ones: an owner who can still sign does not need
recovering, and a recovery set that colludes, or has its keys stolen, must not be able to take the
account. Not freezing keeps a recovery set from locking an owner out.
**Alternatives:** The genesis model — the old set frozen, no veto (right where the old set is assumed
compromised; wrong for a person's account, where it hands the account to the guardians).

## AR003 — The set lives in the wallet, proven with it

**Date:** 2026-10-19
**Decision:** `Wallet.recovery` (the set, the delay, any pending announcement), in the account's leaf.
**Rationale:** Under the state root, an announcement is proven like a balance: the owner's wallet shows
it without believing a node — what makes the veto work in practice.
**Alternatives:** In the account's attachment (not chain-owned; renewals carry attachments as opaque
bytes).

## AR004 — A record of its own, signed by the recovery set

**Date:** 2026-10-19
**Decision:** `T_ACCOUNT_RECOVERY` (21), signed by the account's recovery set; its fee from the account.
One announcement at a time.
**Rationale:** The signers are not the account's keys, so no existing record fits; the account pays as
for anything done to it; one at a time keeps the owner's view simple.
**Alternatives:** A `T_USER_UPDATE` the recovery set may sign (blurs who acts; the update rules would
grow a second signer set).

## AR005 — Applied at an epoch's first block, after its records

**Date:** 2026-10-19
**Decision:** Due announcements apply after the records of an epoch's first block (or the first a node
holds), in place; the accounts are looked through once an epoch.
**Rationale:** Deterministic on every node, as miner liveness. After the records, not before: a
renewal in that block is built from the keys before, and an owner's cancelling update in it wins. Once
an epoch, an in-memory pass over the accounts is cheap; an index would have to follow every rollback
of the account state.
**Alternatives:** Before the records (as first designed: refuses that block's renewals); an index by
`applyEpoch`.

## AR006 — Chain-owned, carried unchanged

**Date:** 2026-10-19
**Decision:** Renewals carry `Wallet.recovery` as it is; the owner's update may set the set and delay
and clears any pending announcement; account creation may name a set, never a pending announcement.
**Rationale:** As miner state: what the chain decides cannot be rewritten by a renewal's leader or an
update's author.

## AR007 — No migration

**Date:** 2026-10-19
**Decision:** The account record changes form; networks start from a new genesis. None is live.
**Rationale:** The owner's call: nothing released runs on these records.
