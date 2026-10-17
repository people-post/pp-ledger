# Current state — load resilience

| Capability | Status | Notes |
|------------|--------|-------|
| Design (lr0) | **Yes** | [DESIGN.md](DESIGN.md): goals 1 (survive overload) and 2 (absorb bursts), pass criteria, three axes (requests, data, topology), tiers; open questions listed there |
| Metrics for the checks (lr1) | **Yes** | `HEALTH` and pp-http `/metrics`: requests served per type (queue wait, handler time), busy / refused / expired; run-loop turns over a second and the longest recent turn (the node's own work delayed); resident memory; miner blocks sealed / committed / abandoned and seal delay; beacon pool size, bytes, oldest age and what became of submissions — [CONFIGURATION.md § Health and metrics](../../docs/ops/CONFIGURATION.md#health-and-metrics). Latency as clients see it is the load generator's (lr2) |
| Load generator (lr2) | **Yes** | `pp-client load-setup` (funded accounts from a funding account) and `pp-client load` (open loop, mixes incl. junk, bursts, many identities, every reply classified, transfers followed into blocks, `--json`) — [TEST_STRATEGY.md § Load generator](../../docs/ops/TEST_STRATEGY.md#load-generator). First runs: [DESIGN.md § Measured](DESIGN.md#measured-first-runs-lr2) |
| Burst baseline (lr3) | Started | Manual runs M1–M6 ([DESIGN.md § Measured](DESIGN.md#measured-first-runs-lr2)): servers survive; bursts are bounced at the first hop, half-delivered with wrong answers, or starved by renewals. Not yet a scripted suite |
| Pool (lr4, lr5) | No | `TxPool`: 10,000 records / 16 MiB / 600 s TTL, admits what decodes, drops on inclusion or expiry; pull of 1,000 records |
| Relay forwarding (lr6) | No | One upstream request per `TX_ADD` |
| Burst suite (lr7) | No | |
| Overload per server (lr8) | No | Bounded queue with lanes, per-peer caps, "busy" before queueing, expiry, `HEALTH` off the queue — in place, untested under load |
| Data axis (lr9) | No | |
| Topology axis (lr10) | No | |
| Promoted to docs (lr11) | No | |
