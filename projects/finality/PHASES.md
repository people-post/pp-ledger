# Finality — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [x] **fn0** Design: a finality bound in the config, kept per block; rewinds on their own above the finalized height, deeper an operator's emergency; a verifying gateway in pp-http (this folder)
- [x] **fn1** The bound in the chain: `finalitySeconds` (config, init-config, validation 1 h – 30 days, a new genesis meta version); a block's finality time and the node's finalized height; `rewind::schedule` on the finalized height instead of seven days; `STATUS` `finalizedHeight`, `finalitySeconds`; `RewindReport.crossedFinality`; tests: a raised bound reopens nothing, a lowered one is prefix-closed, a rewind below the finalized height held for approval
- [x] **fn2** The gateway: pp-http `/v1/` — pinned genesis, authority replay, verified tips and blocks, held certificates and evidence; watched accounts proven when touched, balances by height in its data directory, dropped above a rewind; events; localhost, optional token; tests against a local network (a deposit seen at tip then final; a takeover's rewind reported)
- [x] **fn3** Docs: WIRE_SCHEMA (config, STATUS), BEACON_AUTHORITY § Rewind, a contract for the gateway's API, CONFIGURATION, SETUP (running it beside a service)
