# Current state — load resilience

| Capability | Status | Notes |
|------------|--------|-------|
| Design (lr0) | **Yes** | [DESIGN.md](DESIGN.md): goals 1 (survive overload) and 2 (absorb bursts), pass criteria, three axes (requests, data, topology), tiers; open questions listed there |
| Metrics for the checks (lr1) | **Yes** | `HEALTH` and pp-http `/metrics`: requests served per type (queue wait, handler time), busy / refused / expired; run-loop turns over a second and the longest recent turn (the node's own work delayed); resident memory; miner blocks sealed / committed / abandoned and seal delay; beacon pool size, bytes, oldest age and what became of submissions — [CONFIGURATION.md § Health and metrics](../../docs/ops/CONFIGURATION.md#health-and-metrics). Latency as clients see it is the load generator's (lr2) |
| Load generator (lr2) | No | |
| Burst baseline (lr3) | No | Expected findings from the code in [DESIGN.md § Where we start](DESIGN.md#where-we-start) |
| Pool (lr4, lr5) | No | `TxPool`: 10,000 records / 16 MiB / 600 s TTL, admits what decodes, drops on inclusion or expiry; pull of 1,000 records |
| Relay forwarding (lr6) | No | One upstream request per `TX_ADD` |
| Burst suite (lr7) | No | |
| Overload per server (lr8) | No | Bounded queue with lanes, per-peer caps, "busy" before queueing, expiry, `HEALTH` off the queue — in place, untested under load |
| Data axis (lr9) | No | |
| Topology axis (lr10) | No | |
| Promoted to docs (lr11) | No | |
