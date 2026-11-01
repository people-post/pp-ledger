# Current state — epoch seed

| Capability | Status | Notes |
|------------|--------|-------|
| Seed v2 and beacon hash chain | **Yes** | `consensus::deriveEpochSeedV2`, `beaconSeedStep` / `beaconSeedValue` / `verifyBeaconSeedReveal`; no block hashes, no stake hash — [WIRE_SCHEMA.md § Epoch seed](../../docs/contracts/WIRE_SCHEMA.md#epoch-seed) |
| Certificate v3 | **Yes** | `seedAnchor`, `seedAnchorEpoch`, `seedChainLength` (1 to 100000; default 10000) — [BEACON_AUTHORITY.md § Seed chain](../../docs/contracts/BEACON_AUTHORITY.md#seed-chain) |
| `T_BEACON_SEED` (22) | **Yes** | The reveal for epoch E, in a block of E-1, signed by the beacon key; one per epoch; allowed in a renewals-only block |
| Committee lag | **Yes** | E's committee is the state after the last block of epochs ≤ E-2; a checkpoint joiner trusts the opening block's committee for its first Full epoch; replay uses each block's live admission mode |
| Beacon side | **Yes** | `--init` / `--plan-genesis` write `keys/beacon-seed.secret`; the beacon pools its reveal each epoch; `keys/beacon-seed.next.secret` for a promoted generation |
| CLI | **Yes** | `pp-client beacon-seed-keygen`, `mk-beacon-cert --seed-anchor / --seed-anchor-epoch / --seed-chain-length`; the anchor shows in the certificate's display |
| Compatibility | New networks only | Old certificates, chains and ledgers are not readable ([RE_GENESIS.md](../../docs/ops/RE_GENESIS.md)) |

Implementation is complete ([PLAN.md](PLAN.md)). The final-review fix wave closed: leaders keep the
pooled reveal a place ahead of waiting renewals (F1); a node elects no one when an epoch's committee is
unknown instead of keeping the previous one (F6); a seed-agreement test across a node from genesis, a
node reloaded from 0 after every block and checkpoint joiners, over an empty epoch and two
re-certifications with new anchors (F3, which also covers the `BeaconRevealChain` guard across a new
anchor); known-answer vectors for `beaconSeedStep`, `beaconSeedValue` and `deriveEpochSeedV2` (F7); the
beacon's seed caches keyed on (generation, anchor, target) and (anchor, length) (F8); `mk-beacon-cert`
refuses seed flags without `--seed-anchor` (F9); docs for the first-Full-epoch wording and the
producer's include/omit bit (F4, F5).

Open follow-ups:

- **Accepted gap: a miner restarted in a rotation epoch cannot open the next epoch (F2).** Its replay
  admits the rotation block in CheckpointReplay, so it holds no snapshot of `full`'s start and cannot
  seal `full`+1's opening; if every producer is in that state, `full`+1 stays empty and the network
  recovers at `full`+2 (committee from the bank). Documented in
  [BLOCK_PIPELINE.md](../../docs/architecture/BLOCK_PIPELINE.md#checkpoints-and-late-join) and the ops
  troubleshooting; tested by `CommitteeLagTest.AMinerRestartedInItsRotationEpochRecoversAnEpochLater`.
  Idea to close it, to be evaluated separately: take the snapshot of `full` at the rotation block C′
  itself, if the warm-up state is exact there (C′'s stateRoot is checked Full right after).
- **Live-fleet smoke not run.** `l1` / `smoke` need bash 4+ (the dev Mac has 3.2): seed advancement on a
  running beacon, relays and miners is covered by unit tests only.
- **No test for the beacon's cache keys (F8).** A mid-epoch re-certification held by one live
  `BeaconServer` process needs a hook to commit a block into a running server; not added (test hooks
  in the production API are already a deferred minor).
- **Deferred minor findings:**
  - `deriveEpochSeedV2` does not length-prefix or size-check `prevSeed` / `reveal` (safe while callers
    verify; the layout is now pinned by known-answer vectors).
  - Missing tests: `seedChainLength == 1`; a valid non-empty-anchor genesis certificate; soft
    (CheckpointReplay) recording of an unchecked reveal; the late-block drop of both committee maps,
    and liveness-deregistration timing.
  - The pool path checks a reveal against the committed authority, so a certificate and a reveal in
    one draft fail at the leader's seal (same as `T_RECOVERY`).
  - Seed secret file permissions are not checked on load.
  - Test hooks in the production API (`openForTest`, `pooledForTest`, `submitBeaconSeedIfDue`).
  - Pruning removes epoch K, one more than the brief; `AccountPolicy.cpp` includes `EpochSeed.h` for
    one constant; possibly unused includes; the billable meta size is written twice; identical
    explicit cases in `Operation.cpp`.
