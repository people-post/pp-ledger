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
- [ ] **ba5b** Checkpoint start with a verifiable authority. A miner mounts at the upstream's
      previous checkpoint (`lastId`) and warms up, trusting blocks blindly, until the current one
      (`currentId`); every account renews in that window, the genesis account included, and its
      renewal carries config and authority. Steps:
  - [x] **ba5b.1** `AUTHORITY_PROOF`: beacon and relays index the records that change the authority
        (`T_CONFIG`, `T_BEACON_CERT`) and serve them up to a block; `AuthorityTrail::replayProof`
        replays them from genesis, following admin rotations
  - [ ] **ba5b.2** Warm-up works: soft replay runs without config until the genesis renewal, which
        installs config, consensus and authority when the node has none; no `stateRoot` check
        while soft. Oracle: a node joining at a checkpoint accepts the next Full blocks (same
        state as a node from genesis), live and after a restart
  - [ ] **ba5b.3** Fresh-node sync commits nothing before the tip verifies: the whole range is
        authenticated from its first block; a trail with no authority yet takes it from the first
        genesis record or renewal, and later ones must agree
  - [ ] **ba5b.4** Anchors: a miner pinning genesis checks the taken authority and admin set against
        the replayed proof; relays can pin `networkAnchor.genesisHash` too
  - [ ] **ba5b.5** Smoke: miners join at a checkpoint above 0, pinned and unpinned; docs, with the
        known limits (a stale key with withheld blocks; warm-up state not re-validated)
- [ ] **ba5c** Certificates out of band: the current certificate on `STATUS` (B012), accepted from
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
