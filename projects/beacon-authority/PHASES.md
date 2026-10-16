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
- [x] **ba5b** Checkpoints a miner can start from, with a verifiable authority (B015). A checkpoint
      is the first block of an epoch, opens with the genesis renewal, and every epoch's first block
      carries its committee. A miner starts at the previous checkpoint, warms up blindly, and works
      (checks everything, seals) from the current one. Steps:
  - [x] **ba5b.1** `AUTHORITY_PROOF`: beacon and relays index the records that change the authority
        (`T_CONFIG`, `T_BEACON_CERT`) and serve them up to a block; `AuthorityTrail::replayProof`
        replays them from genesis, following admin rotations
  - [x] **ba5b.2** Checkpoints at epoch starts, opening with the genesis renewal; a node starting
        there installs config, consensus, authority and the genesis account from it. Renewal
        due-sets and rotation read slot times, not the node's clock (they were nondeterministic)
  - [x] **ba5b.3** Each epoch's first block carries its committee (preimage of `stakeSnapshotHash`):
        Full nodes check it; warm-up uses it, so leaders and liveness charges are exact
  - [x] **ba5b.4** Warm-up is exact. Oracle with skipped slots and a miner registering mid-window:
        the joiner validates Full blocks from the current checkpoint, live and after a restart
  - [x] **ba5b.5** Fresh-node sync commits nothing before the tip verifies: the whole range is
        authenticated; the authority comes from the starting block's genesis record or renewal
  - [x] **ba5b.6** Anchors: a miner pinning genesis checks the starting block's authority and admin
        set against the replayed proof; relays can pin `networkAnchor.genesisHash` too
  - [x] **ba5b.7** Smoke: miners join at a checkpoint above 0, pinned and unpinned; docs (late
        join, checkpoint rules), with the known limits. On the way (B016): warm-up applies every
        effect on held accounts (a soft-mode audit), `T_END_USER` carries its written-off balances,
        a joined miner seals once its history covers the transaction validity window, and a
        self-transfer (it minted) is refused
- [x] **ba5c** Certificates out of band: the current certificate on `STATUS` (B012), accepted from
      any peer or a file, the highest one seen persisted; `cutoverHeight` against a generation not
      yet on chain (needed by part 2's case 2) (B017). Steps:
  - [x] **ba5c.1** Cutover semantics fixed first: on chain a certificate commits by the block after
        its cutover (the old rule — cutover below the commit — let the old generation sign past it,
        and would deadlock an honest rotation once nodes enforce cutovers); the cutover in force is
        the block before the commit; `mk-beacon-cert` without `--cutover` means none
  - [x] **ba5c.2** `Chain::offerCertificate`: the highest admin-signed certificate seen, dropped once
        committed or unsigned by an admin rotation; `AuthorityTrail` refuses older generations'
        tips above its cutover
  - [x] **ba5c.3** Channels: `STATUS.certificate` (offered by `BlockSync` before the tip, and by a
        miner choosing upstreams); `TX_ADD` keeps it at every node it passes; `CertificateStore`
        persists it (`data/beacon-cert.dat`), also for a node that has no block yet
  - [x] **ba5c.4** The beacon: stands down when a seen certificate names a key it does not hold;
        holding it, commits nothing past the cutover but a certificate. Relays stop serving a tip
        it rules out
  - [x] **ba5c.5** Tests (chain, sync, beacon, store; mutation-checked), smoke
        (L-SMOKE-OUT-OF-BAND in `--suite rotation`), docs
- [x] **ba6** Equivocation halt: detection, evidence relay, halt and resume (B018). Steps:
  - [x] **ba6.1** Evidence (`Client::Equivocation`: two tips, one height, different blocks) and the
        halt: `Chain::offerEquivocation` checks it against the generation's key; `AuthorityTrail`
        refuses that generation's tips above its height; a later certificate resumes.
        `UpstreamSignedTip` conflicts by block hash only
  - [x] **ba6.2** Storage and roles: `EquivocationStore` (`data/equivocation.dat`, shared `NodeFile`
        helpers); relays and miners build evidence from conflicting tips; the beacon refuses
        commits above it and producer writes (not a certificate); miners stop producing
  - [x] **ba6.3** Channels: `STATUS.equivocation` down, `EQUIVOCATION_ADD` up (kept and forwarded
        at each node; pushed up by the node that found it); `pp-client mk-equivocation` /
        `add-equivocation`; status shows it
  - [x] **ba6.4** Tests (chain, store, beacon, sync, wire; mutation-checked), smoke
        (L-SMOKE-EQUIVOCATION in `--suite rotation`), docs
- [x] **ba7** Recovery action: announce, delay, apply (replace the admin set); admin set frozen while
      pending; pending recovery on `STATUS` / logs / alert (B019). Steps:
  - [x] **ba7.1** `T_RECOVERY` signed by the recovery set; pending in `ChainAuthority` (genesis meta
        v9); freeze of `T_CONFIG` / `T_BEACON_CERT` (also out of band); apply in place at the first
        block of `applyEpoch`; `AuthorityTrail` and proofs (steps carry epochs) follow it. A genesis
        renewal must carry the admin set in force (closes a leader-signed key swap)
  - [x] **ba7.2** `STATUS.recovery`, `pp-client status`, per-epoch alerts; `pp-client mk-recovery`;
        renewals built and checked as the block's epoch holds the authority (found by the smoke)
  - [x] **ba7.3** Tests (chain, renewal, proof replay, wire; mutation-checked), smoke
        (L-SMOKE-RECOVERY in `--suite rotation`), docs
- [x] **ba8** Promote wire / disk changes into `docs/contracts/` (B020). Steps:
  - [x] **ba8.1** The authority's rules move out of WIRE_SCHEMA into their own contract,
        `BEACON_AUTHORITY.md`, with a formats / versions table and the RPC changes; the node state
        files join `LEDGER_STORAGE.md`; stale architecture text (security model, open item E)
        updated; decisions marked superseded
  - [x] **ba8.2** Every contract claim checked against the code. Fixed where the code was wrong: a
        block holding two certificates was accepted (only a producer's draft refused it); a
        certificate's file stayed on disk until restart after the chain dropped it. Corrected
        where the docs were: miner `STATUS`, start-only upstream ranking, fresh-node first sync,
        write order and durability of the state files

## Part 2 — promotion (design after part 1)

- [ ] **bp0** Design note: bounded rewind to `cutoverHeight`, beacon candidate lists, clock carry-over,
      pool / registry rebuild
- [ ] **bp1** Case 2 (beacon stolen) procedure and tooling
- [ ] **bp2** Case 3 (move the server) runbook
- [ ] **bp3** Case 1 (re-genesis) runbook
