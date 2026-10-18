# Current state — account recovery

| Capability | Status | Notes |
|------------|--------|-------|
| Design (ar0) | Done | [DESIGN.md](DESIGN.md); AR001–AR008: opt-in recovery sets for user and issued accounts, standing above the account's keys (an announcement freezes the account; only the set withdraws it or changes itself), the delay ending at an epoch start, proven in the account's leaf. First built with the owner's veto; revised before any release |
| State and rules (ar1) | Done | `Wallet.recovery` (`UserAccount` v3), `T_ACCOUNT_RECOVERY` (21, `AccountRecoveryTxHandler`: announce, withdraw, change), `validateRecoverySet`, `recoveryApplyEpoch`, the freeze (`RecordHandler::checkNotFrozen`), `Chain::applyDueAccountRecoveries`; tests `AnAccountsRecoverySetStandsAboveItsKeys`, `AccountRecoveryPolicyTest` |
| Operation files, pp-client (ar2) | Done | Kind `account-recovery`; `op::setRecovery` (the first set), `announceRecovery`, `withdrawRecovery`, `changeRecovery`; `pp-client op set-recovery` (the first by the account's keys, then a change by the set), `recover`, `withdraw-recovery`; L1 smoke `run_account_recovery_scenario`; WIRE_SCHEMA § Account recovery, OPERATION_FILE |
| pp-ledger-apps (ar3) | Done (pp-ledger-apps, local) | The wallet's Recovery screen (name the first set; for recovery-key holders: announce, withdraw, change the set) and the frozen account on Accounts, proven; the token app's Signers and Overview; chain smoke 6b: set, announced from a second wallet holding the set, frozen on Accounts, withdrawn; pp-ledger-apps WL010 |
| Docs (ar4) | Done | WIRE_SCHEMA § Account recovery (AR001–AR006 superseded by it), OPERATION_FILE, TEST_STRATEGY L-SMOKE-RECOVERY; pp-ledger-apps' wallet and token guides |
