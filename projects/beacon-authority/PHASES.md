# Beacon authority — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

## Part 1 — the real beacon

- [x] **ba0** Settle DESIGN open questions (B006–B009)
- [x] **ba1** Genesis trust roots (breaking): recovery set (keys, threshold, delay in epochs) in
      genesis; `T_CONFIG` cannot change it; `--init` generates `keys/beacon-signing.key` and the
      generation-1 certificate
- [x] **ba2** Beacon certificate record and verification (genesis → admin set → certificate):
      `T_BEACON_CERT`, `Chain::verifyBeaconCertificateRecord`, `pp-client mk-beacon-cert`
- [x] **ba3** Signed tip: beacon signs after durable commit, persists last signed height; carried
      on `STATUS` / `BLOCK_WAIT` / `BLOCK_ADD`; relays pass it through
- [x] **ba4** Adoption rule: relays and miners commit only up to a verified signed tip; a miner
      commits its own block only on the beacon's signed tip
- [x] **ba5** Generations on chain (B014): nodes follow the generation the admin set certifies
      along the blocks (`AuthorityTrail`); older generations superseded; same-server rotation
      (`keys/beacon-signing.next`); a beacon whose key is no longer certified stands down (reads
      only); miners prefer the highest generation; an anchored miner pins its first block
- [ ] **ba5b** Certificates out of band: the current certificate on `STATUS` (B012), accepted from
      any peer or a file, the highest one seen persisted; `cutoverHeight` against a generation not
      yet on chain (needed by part 2's case 2)
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
