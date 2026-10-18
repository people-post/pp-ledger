# Account recovery — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [x] **ar0** Design: the recovery set, announce, cancel, apply (this folder)
- [x] **ar1** The state and the rules: `Wallet.recovery`; `T_ACCOUNT_RECOVERY`; an update sets the set and clears a pending announcement; due announcements applied after the records of an epoch's first block; renewals carry it; unit tests
- [x] **ar2** Operation files and pp-client: kind `account-recovery`; `op set-recovery`, `op cancel-recovery`, `op recover`; WIRE_SCHEMA, OPERATION_FILE; L1 smoke (set, announce, cancel, announce, apply, the new key signs)
- [x] **ar3** pp-ledger-apps: the wallet's Recovery (set, cancel, recover) and the pending warning, proven; the token app's; chain smoke
- [x] **ar4** Docs: the wallet's and token app's guides; this project closed into `docs/contracts/`
