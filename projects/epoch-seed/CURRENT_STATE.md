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

Implementation is complete ([PLAN.md](PLAN.md)). Open follow-ups:

- **Live-fleet smoke not run.** `l1` / `smoke` need bash 4+ (the dev Mac has 3.2): seed advancement on a
  running beacon, relays and miners is covered by unit tests only.
- **Deferred minor findings:**
  - No known-answer vectors pin `beaconSeedStep` and the v2 preimage bytes; `deriveEpochSeedV2` does
    not length-prefix or size-check `prevSeed` / `reveal` (safe while callers verify).
  - Missing tests: `seedChainLength == 1`; a valid non-empty-anchor genesis certificate; the
    `BeaconRevealChain` guard across a certificate with a new anchor; soft (CheckpointReplay)
    recording of an unchecked reveal; a direct test of the provisional committee, the late-block
    drop of both committee maps, and liveness-deregistration timing.
  - The pool path checks a reveal against the committed authority, so a certificate and a reveal in
    one draft fail at the leader's seal (same as `T_RECOVERY`).
  - The provisional path keeps the previous committee installed when `committeeFor` errors.
  - The beacon's cached reveal record is keyed on the target epoch only, so it goes stale if the
    certificate rotates mid-epoch (one absent-seed epoch); the anchor-match cache is keyed on the
    anchor, not (anchor, length).
  - Seed secret file permissions are not checked on load.
  - Test hooks in the production API (`openForTest`, `pooledForTest`, `submitBeaconSeedIfDue`).
  - Pruning removes epoch K, one more than the brief; `AccountPolicy.cpp` includes `EpochSeed.h` for
    one constant; possibly unused includes; the billable meta size is written twice; identical
    explicit cases in `Operation.cpp`.
