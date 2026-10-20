# Current state — finality

| Capability | Status | Notes |
|------------|--------|-------|
| Design (fn0) | Done | [DESIGN.md](DESIGN.md); FN001–FN006. First consumer: the bank's pp-ledger deposits (`nfsc-brief-cashier/app-cashier`, projects/bank b4) |
| The bound in the chain (fn1) | Done (local branch) | `BlockChainConfig.finalitySeconds` (genesis meta v14, StateSnapshot v3), default a day, `chain_tx::validateFinality` (1 h – 30 days) at `--init` and `T_CONFIG`; `Chain::finalityBounds_` (the bound by epoch, noted at each committed block), `finalitySecondsAt`, `finalizedHeight` (bisection per stretch of one bound; prefix-closed); `rewind::schedule` on `rewindCrossesFinality` (the seven-day constant gone), an approved crossing written `crossedFinality` (`RewindReport` v2); `STATUS` `finalizedHeight`, `finalitySeconds`; `pp-client -b status` prints them. Tests: `Finality_ABlockKeepsTheBoundItWasCommittedUnder` (raised, lowered, prefix-closed), `ARewindPastFinalityWaitsForApproval`, limits at `--init` and `T_CONFIG`; the full suite (627) passes. Contracts updated (WIRE_SCHEMA, BEACON_AUTHORITY, CONFIGURATION, LEDGER_TOPOLOGY); FN001–FN003 superseded by them |
| The gateway (fn2) | No | Today: pp-http passes node answers on, unverified; `pp-client verified-account` shows the checks |
| Docs (fn3) | No | |
