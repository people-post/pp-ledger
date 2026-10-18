# Current state — account recovery

| Capability | Status | Notes |
|------------|--------|-------|
| Design (ar0) | Done | [DESIGN.md](DESIGN.md); AR001–AR007: opt-in recovery sets for user and issued accounts, the owner's veto (no freeze), proven in the account's leaf |
| State and rules (ar1) | Done | `Wallet.recovery` (`UserAccount` v3), `T_ACCOUNT_RECOVERY` (21, `AccountRecoveryTxHandler`), `validateOwnerRecovery`, `Chain::applyDueAccountRecoveries`; tests `AnAccountIsRecoveredUnlessItsOwnerCancels`, `AccountRecoveryPolicyTest` |
| Operation files, pp-client (ar2) | Done | Kind `account-recovery`; `op::setRecovery`, `cancelRecovery`, `announceRecovery` (an owner's update never carries an announcement); `pp-client op set-recovery`, `cancel-recovery`, `recover`; L1 smoke `run_account_recovery_scenario`; WIRE_SCHEMA § Account recovery, OPERATION_FILE |
| pp-ledger-apps (ar3) | Done (pp-ledger-apps, local) | The wallet's Recovery screen (set or remove, cancel, recover an account) and the announcement on Accounts, proven; the token app's Signers (set, remove, cancel) and Overview; chain smoke 6b: set, announced from a second wallet holding the set, cancelled; pp-ledger-apps WL010 |
| Docs (ar4) | Done | WIRE_SCHEMA § Account recovery (AR001–AR006 superseded by it), OPERATION_FILE, TEST_STRATEGY L-SMOKE-RECOVERY; pp-ledger-apps' wallet and token guides |
