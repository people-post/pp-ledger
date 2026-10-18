# Account recovery — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [ ] **ar0** Design: the recovery set, announce, cancel, apply (this folder)
- [ ] **ar1** The state and the rules: `Wallet.recovery`; `T_ACCOUNT_RECOVERY`; an update sets the set and clears a pending announcement; due announcements applied at an epoch's first block from an index; renewals carry it; unit tests
- [ ] **ar2** Operation files and pp-client: kind `account-recovery`; `op update-keys` with a recovery set; `op recover`; L1 smoke (set, announce, cancel, announce, apply, the new key signs)
- [ ] **ar3** pp-ledger-apps: the wallet's Recovery (set, cancel, recover) and the pending warning, proven; the token app's; chain smoke
- [ ] **ar4** Docs: WIRE_SCHEMA, OPERATION_FILE; the wallet's guide
