# Current state — finality

| Capability | Status | Notes |
|------------|--------|-------|
| Design (fn0) | Done | [DESIGN.md](DESIGN.md); FN001–FN006. First consumer: the bank's pp-ledger deposits (`nfsc-brief-cashier/app-cashier`, projects/bank b4) |
| The bound in the chain (fn1) | No | Today: `rewind::kMaxAutomaticRewind` (seven days), deeper with `--approve-rewind` |
| The gateway (fn2) | No | Today: pp-http passes node answers on, unverified; `pp-client verified-account` shows the checks |
| Docs (fn3) | No | |
