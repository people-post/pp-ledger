# Beacon authority — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## Part 1 — the real beacon

- [x] **ba0** Settle DESIGN open questions (B006–B009)
- [ ] **ba1** Genesis trust roots (breaking): recovery set (keys, threshold, delay in epochs) in
      genesis; `T_CONFIG` cannot change it; `--init` generates `keys/beacon-signing.key` and the
      generation-1 certificate
- [ ] **ba2** Beacon certificate record and verification (genesis → admin set → certificate)
- [ ] **ba3** Signed tip: beacon signs after durable commit, persists last signed height; carried
      on `STATUS` / `BLOCK_WAIT` / `BLOCK_ADD`; relays pass it through
- [ ] **ba4** Adoption rule: relays and miners commit only up to a verified signed tip; a miner
      commits its own block only on the beacon's signed tip
- [ ] **ba5** Generations: follow the highest valid generation; `cutoverHeight`; a beacon that
      sees a higher generation steps down to relay
- [ ] **ba6** Equivocation halt: detection, evidence relay, halt and resume
- [ ] **ba7** Recovery action: announce, delay, apply (replace the admin set); admin set frozen while
      pending; pending recovery on `STATUS` / logs / alert
- [ ] **ba8** Promote wire / disk changes into `docs/contracts/`

## Part 2 — promotion (design after part 1)

- [ ] **bp0** Design note: bounded rewind to `cutoverHeight`, beacon candidate lists, clock carry-over,
      pool / registry rebuild
- [ ] **bp1** Case 2 (beacon stolen) procedure and tooling
- [ ] **bp2** Case 3 (move the server) runbook
- [ ] **bp3** Case 1 (re-genesis) runbook
