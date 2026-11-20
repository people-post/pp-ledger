# Beacon Epoch Seed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop slot leaders from grinding the next epoch's seed. The seed mixes in a value from a beacon hash chain, and each epoch's committee is taken one epoch earlier.

**Architecture:**

- **The beacon secret.** The beacon holds a secret `s`. Its admin-signed `BeaconCertificate` commits to `anchor = Hᴺ(s)`.
- **The reveal.** During epoch E−1 the beacon submits a `T_BEACON_SEED {epoch E, reveal}` record, signed with its certified key. The reveal is `Hᴺ⁻ᵏ(s)`, where k = E − `seedAnchorEpoch`. Miners include it like any pool record.
- **The seed.** The seed for E is H(v2, E, networkId, seed of E−1, reveal of E or "absent"). It no longer uses block hashes or the stake hash.
- **The committee.** E's committee is the stake state after the last block of epochs ≤ E−2, so nobody can rearrange it after the reveal is known.

**Tech Stack:** C++20, CMake/Ninja, GoogleTest, ML-DSA-65 and SHA-256 via pp-cpp-crypto / `utl::`.

**Spec:** the pp-ledger code review doc, sections "S2 in detail" and "S2 fixes 1 and 2 in depth" (Option 1).

## Global Constraints

- **Fork-critical.** This is for new networks only. `BeaconCertificate::VERSION` goes 2 → 3, and old certificates and chains are not readable. `Block::CURRENT_VERSION` stays 9.
- **New record type** `Ledger::T_BEACON_SEED = 22`. Bump `kNumTxTypes` to `T_BEACON_SEED + 1`.
- **Hash step:** `H(x) = utl::sha256Raw("pp-ledger/beacon-seed/v1" || x)`. Reveals are 32 raw bytes.
- **Seed domain:** `"pp-ledger/epoch-seed/v2"`. The genesis seed (`epoch-seed/genesis/v1`) is unchanged.
- **Default chain length:** N = `consensus::kDefaultBeaconSeedChainLength = 10000` epochs. The maximum accepted in a certificate is `kMaxBeaconSeedChainLength = 100000`.
- **An empty `seedAnchor` is legal.** It means the beacon contributes nothing, and every epoch uses the "absent" seed.
- **The beacon secret** lives in `keys/beacon-seed.secret` (hex, 32 bytes), created with `utl::writeToNewFile` (0600). An optional `keys/beacon-seed.next.secret` is used for promotion.
- **Code style.** Match the surrounding code: 2-space indent, sparse comments that explain why, and `chain_tx::TxError(chain_err::E_TX_VALIDATION, ...)` for refusals.
- **Testing.** Write each test first and watch it fail. Run the full `ctest` suite before every commit.

## Review Focus

1. **Late reveal.** The reveal lands in E−1's last block, which arrives after the clock has entered E and a provisional seed for E is already cached. The first-slot leader of E must still elect with the corrected seed. Test in Task 4.
2. **Beacon restart mid-epoch.** The beacon restarts after its reveal was pooled but before it was included. It must resubmit without producing a second reveal for the epoch. Test in Task 6.
3. **Renewals-only block.** A block that has to leave due renewals out must still be able to carry the reveal. Test in Task 3.
4. **Checkpoint joiner.** Its first Full epoch can't recompute E−1's snapshot from its own state. It takes that committee from the block, and from the next epoch on it computes exactly. Covered by `CheckpointJoinTest.*`, plus a test in Task 5.
5. **Missing or exhausted chain.** A certificate with an empty anchor, or an epoch past `seedAnchorEpoch + seedChainLength`, must give "absent" seeds with no errors, and any reveal for it is refused. Test in Task 3.

---

### Task 1: Seed v2 and hash-chain primitives

**Files:**
- Modify: `include/ledger/consensus/EpochSeed.h`, `src/consensus/EpochSeed.cpp`
- Create: `src/consensus/test/test_epoch_seed.cpp`, and add it to `src/consensus/test/CMakeLists.txt` the same way `test_slot_committee` is added.

**Interfaces (produces):**
- `std::string beaconSeedStep(const std::string &value);` — one H step.
- `std::string beaconSeedValue(const std::string &secret, uint64_t steps);` — Hˢᵗᵉᵖˢ(secret).
- `bool verifyBeaconSeedReveal(const std::string &anchor, uint64_t steps, const std::string &reveal);` — checks Hˢᵗᵉᵖˢ(reveal) == anchor. False if `reveal.size() != 32` or `steps == 0`.
- `std::string deriveEpochSeedV2(uint64_t epoch, const std::string &networkId, const std::string &prevEpochSeed, const std::optional<std::string> &beaconReveal);`
- The constants `kDefaultBeaconSeedChainLength = 10000` and `kMaxBeaconSeedChainLength = 100000`.

- [ ] **Step 1: Write failing tests** in `test_epoch_seed.cpp`:
  - `BeaconSeedChain.RevealForStepKHashesToTheAnchorInKSteps`: with `s` = 32 bytes of `0x01` and N = 5, for k = 1..5, `verifyBeaconSeedReveal(beaconSeedValue(s,5), k, beaconSeedValue(s,5-k))` is true, and it is false with k+1.
  - `BeaconSeedChain.RefusesShortRevealsAndZeroSteps`
  - `EpochSeedV2.DependsOnTheRevealAndItsAbsence`: the seeds for `reveal=r1`, `reveal=r2` and `nullopt` are all different, and each is 32 bytes.
  - `EpochSeedV2.IsDeterministic`
- [ ] **Step 2:** Run `ctest --test-dir build -R 'BeaconSeedChain|EpochSeedV2'`. Expected: build fails with the symbols undefined.
- [ ] **Step 3:** Implement the functions. The v2 preimage is the domain, `u64 epoch`, `u64 len(networkId)`, networkId, prevSeed, a `u8` flag (1 if a reveal is present), then the reveal if present. Use `OutputArchive`, as v1 does.
- [ ] **Step 4:** Run the same tests. Expected: PASS.
- [ ] **Step 5:** Commit `feat(consensus): epoch seed v2 and beacon seed hash chain`.

### Task 2: Certificate anchor fields

**Files:**
- Modify: `include/ledger/chain/Types.h:153-189` (`BeaconCertificate`), `src/chain/Types.cpp:77-96`, `src/chain/AccountPolicy.cpp` (`validateGenesisAuthority` ~178, `validateNextBeaconCertificate` ~232)
- Test: `src/chain/test/test_chain.cpp`, near the existing certificate tests.

**Interfaces (produces):**
- `BeaconCertificate` gains `std::string seedAnchor;` (empty or 32 bytes), `uint64_t seedAnchorEpoch{0};` and `uint64_t seedChainLength{0};`. They are serialized after `dialHints`, and `VERSION` becomes 3.
- `bool BeaconCertificate::hasSeedChain() const;` — true when `seedAnchor.size() == 32`.

- [ ] **Step 1: Failing tests:**
  - `BeaconCertificateTest.SeedAnchorRoundTrips`
  - `BeaconCertificateTest.RefusesABadSeedAnchor`: an anchor of 31 bytes, a length of 0 with a 32-byte anchor, or a length above `kMaxBeaconSeedChainLength` is refused by `validateGenesisAuthority`, and by `validateNextBeaconCertificate` when the cert is offered.
  - `BeaconCertificateTest.AnEmptyAnchorIsValid`
- [ ] **Step 2:** Run the tests. Expected: they fail.
- [ ] **Step 3:** Add the fields and a `validateSeedAnchor(const BeaconCertificate&)` helper in `AccountPolicy.cpp`, called from both validators.
- [ ] **Step 4:** Run the tests, then the full `test_chain`. Expected: PASS.
- [ ] **Step 5:** Commit `feat(chain): beacon certificate commits to a seed hash chain`.

### Task 3: `T_BEACON_SEED` record

**Files:**
- Modify:
  - `include/ledger/ledger/Ledger.h` and `src/ledger/Ledger.cpp`: the type id, `TxBeaconSeed`, the `TypedTx` alternative, `kTypeByIndex`, the `decode()` case and the human name.
  - `include/ledger/chain/RecordHandler.h:22` and `src/chain/RecordHandler.cpp:26-44`.
  - `include/ledger/chain/TxContext.h` (add `std::map<uint64_t, std::string> beaconReveals;`).
  - `src/chain/Chain.h` / `Chain.cpp`: add `beaconReveals` to `TipSnapshot` and `beginTipUpdate`/`rollbackTipUpdate`, the `validateTxSignatures` branch, and pruning.
  - `src/chain/BlockValidation.cpp`: the renewals-only exemption at :704, and at most one seed record per block in `validateAuthorityRecordCount`.
  - `src/chain/AccountPolicy.{h,cpp}`: `verifyBeaconSigned`.
  - `src/operation/Operation.cpp:225,242`.
- Create: `src/chain/BeaconSeedTxHandler.{h,cpp}`, and add it to `src/chain/CMakeLists.txt`.
- Test: `src/chain/test/test_chain.cpp`, new fixture `BeaconSeedTest` built on `ComposeHarness`. The harness authority gets a seed chain: give `makeTestAuthority` a test secret `testBeaconSeedSecret()`, N = 64 and anchor epoch 0.

**Interfaces (produces):**
- `struct Ledger::TxBeaconSeed { uint64_t epoch{0}; std::string reveal; serialize; ltsToMeta(); }`
- `chain_tx::Roe<void> chain_tx::verifyBeaconSigned(const Ledger::Record &, const BeaconCertificate &, const std::string &networkId, const Crypto &, logging::Logger &);` — exactly one signature, by `cert.publicKey`.
- `BeaconSeedTxHandler`, an `ITxHandler`:
  - `getSignerAccountId` returns `ID_GENESIS`.
  - `getBillableCustomMetaSizeForFee` returns 0.
  - `getIdempotencyKey` returns nullopt.
  - `applyBuffer` / `applyBlock` validate and then record the reveal: `ctx.beaconReveals[epoch] = reveal` in the block path, and a draft tag `"beacon-seed"` in the buffer path.
- Validation, in strict mode:
  - `tx.epoch == epochOf(block slot) + 1`.
  - The certificate in force has a seed chain.
  - `seedAnchorEpoch < tx.epoch <= seedAnchorEpoch + seedChainLength`.
  - `verifyBeaconSeedReveal(anchor, tx.epoch - seedAnchorEpoch, reveal)`. Fast path: if `beaconReveals` holds an earlier epoch e′ under the same anchor, check `verifyBeaconSeedReveal(reveals[e′], tx.epoch - e′, reveal)` instead.
  - No reveal is recorded for `tx.epoch` yet.
- In soft (CheckpointReplay) mode, record the reveal without verifying it.

- [ ] **Step 1: Failing tests** (each builds the record with `makeRecord`-style signing by `testBeaconKey()`):
  - `BeaconSeedTest.TheBeaconsRevealForTheNextEpochIsAdmitted`: `checkAdmission` accepts it, and once a block carrying it is added, `getBeaconReveal(E)` returns it. Add `std::optional<std::string> Chain::getBeaconReveal(uint64_t epoch) const`.
  - `BeaconSeedTest.RefusesAWrongSignerAWrongEpochOrAForgedReveal`
  - `BeaconSeedTest.OneRevealPerEpoch`: a second reveal is refused in the same block and in a later block of the same epoch.
  - `BeaconSeedTest.NoChainOrAnExhaustedChainRefusesReveals`
  - `BeaconSeedTest.ARenewalsOnlyBlockMayCarryTheReveal`
  - `BeaconSeedTest.AFailedBlockLeavesNoReveal`: a block that fails after the reveal applies leaves `getBeaconReveal(E)` empty.
- [ ] **Step 2:** Run `./build/src/chain/test/test_chain --gtest_filter='BeaconSeedTest.*'`. Expected: it fails to compile, then fails.
- [ ] **Step 3:** Implement as specified. Prune `beaconReveals` entries more than `kStakeSnapshotsKept` epochs below the block's epoch.
- [ ] **Step 4:** Run the tests, then the full suite. Expected: PASS.
- [ ] **Step 5:** Commit `feat(chain): T_BEACON_SEED records the beacon's reveal for the next epoch`.

### Task 4: Derive seeds with v2

**Files:**
- Modify: `src/chain/Chain.cpp:1024-1126` (`deriveNextEpochSeed`, `ensureEpochSeed`; remove `collectPrevEpochLookbackHashes` and its use of the stake hash), `src/chain/Chain.h`.
- Test: `src/chain/test/test_chain.cpp` (`BeaconSeedTest`).

**Interfaces:** `std::string Chain::deriveNextEpochSeed(uint64_t epoch, const std::string &prevSeed) const` calls `deriveEpochSeedV2(epoch, networkId, prevSeed, getBeaconReveal(epoch))`.

- [ ] **Step 1: Failing tests:**
  - `BeaconSeedTest.TheNextSeedMixesTheReveal`: the seed of E equals `deriveEpochSeedV2(E, net, seed of E−1, reveal)`.
  - `BeaconSeedTest.WithoutARevealTheSeedIsTheAbsentOne`
  - `BeaconSeedTest.TheLastLeaderCannotSteerTheNextSeed`: build two variants of E−1's last block with different records (one with an extra transfer). Both give the same seed for E. This is the regression test for S2.
  - `BeaconSeedTest.ALateRevealCorrectsTheProvisionalSeed`: advance the clock into E, call `refreshStakeholders()`, add E−1's last block carrying the reveal, and refresh again. `getEpochSeed()` for E equals the reveal-mixed seed.
- [ ] **Step 2:** Run the tests. Expected: they fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the full suite. Fix tests that hard-code v1 seed behavior by updating their expectations to v2 only, and list every changed test in the commit message.
- [ ] **Step 5:** Commit `feat(chain): epoch seed v2 mixes the beacon reveal, not leaders' blocks`.

### Task 5: Committee from one epoch earlier

**Files:**
- Modify: `src/chain/Chain.cpp` (`refreshStakeholders()` ~974, `refreshStakeholders(uint64_t)` ~1007, `prepareBlockEpoch` ~1389, the seal path ~764), `src/chain/Chain.h` (a new map `epochCommittees_`; `epochStakeSnapshots_` keeps meaning "candidates at the start of epoch k").
- Test: `src/chain/test/test_chain.cpp`, new fixture `CommitteeLagTest`.

**Interfaces:**
- `std::vector<consensus::Stakeholder> Chain::committeeFor(uint64_t epoch, const Ledger::Block *opening) const`. Rule:
  - If a snapshot of E−1 is recorded, the committee is `epochStakeSnapshots_[E−1]`.
  - Else, if no block of epoch E−1 is held before `opening`, it is the current candidates (the same state).
  - Else, if `opening` is given, it is `opening->committee` (a joiner's first Full epoch).
  - Else, error (refuse to seal).
- `prepareBlockEpoch`:
  - In Full mode, record the snapshot of E and install `committeeFor(E)`.
  - In warm-up, install the block's committee into `epochCommittees_[E]` and do not record a snapshot.

- [ ] **Step 1: Failing tests:**
  - `CommitteeLagTest.AStakeMoveInTheLastBlockReachesTheCommitteeAnEpochLater`: a miner registered in E−1's last block is not in E's committee but is in E+1's.
  - `CommitteeLagTest.AnEmptyPreviousEpochUsesTheStateBeforeIt`
  - `CommitteeLagTest.AJoinersFirstFullEpochTakesTheBlocksCommittee`
- [ ] **Step 2:** Run the tests. Expected: they fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the full suite. Update the tests whose committee timing moves by one epoch (registration, revoke and liveness deregistration) and list them in the commit message.
- [ ] **Step 5:** Commit `feat(chain): each epoch's committee is fixed one epoch ahead`.

### Task 6: Beacon submits its reveal

**Files:**
- Modify:
  - `src/server/BeaconServer.{h,cpp}`: `--init` creates the secret (~148) and puts the anchor in the genesis cert (~763). `runLoop` (~1202) calls `submitBeaconSeedIfDue()`. Factor `admitToPool(const Ledger::Record&)` out of `dTxAdd` (~1307).
  - `src/server/TipSigner.{h,cpp}`: `Roe<std::string> signMessage(const BeaconCertificate &, const std::string &message) const`.
- Test: `src/server/test/test_beacon_server.cpp`.

**Interfaces:**
- `void BeaconServer::submitBeaconSeedIfDue()`:
  - Once the clock is in epoch E−1, and the chain has no reveal for E, and the cert has a seed chain, and this beacon holds both the cert's key and the secret whose `beaconSeedValue(s, N)` equals the anchor: build, sign and pool the reveal.
  - Retry on each loop turn until the chain records it. Skip when superseded or halted.

- [ ] **Step 1: Failing tests:**
  - `BeaconServerInitTest.InitWritesTheSeedSecretAndTheGenesisCertCommitsToIt`
  - `BeaconSeedSubmitTest.TheBeaconPoolsItsRevealForTheNextEpoch`
  - `BeaconSeedSubmitTest.ARestartResubmitsWithoutASecondReveal`
  - `BeaconSeedSubmitTest.WithoutTheSecretItSubmitsNothing`
- [ ] **Step 2:** Run the tests. Expected: they fail.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** Run the full suite, plus `./scripts/test/pp_ledger_local_test.sh run --suite l1`, to show seeds keep advancing on a live fleet.
- [ ] **Step 5:** Commit `feat(server): the beacon reveals each epoch's seed contribution`.

### Task 7: CLI and docs

**Files:**
- Modify: `src/app/client.cpp` (`mk-beacon-cert` at ~369 gains `--seed-anchor`, `--seed-anchor-epoch` and `--seed-chain-length`; new subcommand `beacon-seed-keygen -o <file> [--length N]`, which writes the secret with `writeToNewFile` and prints the anchor), `src/operation/Operation.cpp` (display the anchor).
- Modify the docs:
  - `docs/contracts/WIRE_SCHEMA.md`: seed v2, the committee lag and record 22.
  - `docs/contracts/BEACON_AUTHORITY.md`: the anchor, and promotion with `beacon-seed.next.secret`.
  - `docs/architecture/BLOCK_PIPELINE.md`: the joiner's first Full epoch.
  - `docs/ops/CONFIGURATION.md`: the secret file.
  - `src/consensus/README.md`.
  - `docs/architecture/SLOT_COMMITTEE_OPEN_ITEMS.md`: mark grinding as closed.
- Test: `src/app` has no unit tests. Verify by running `pp-client beacon-seed-keygen` and `mk-beacon-cert` by hand and pasting the output into the PR.

- [ ] **Step 1:** Implement the CLI.
- [ ] **Step 2:** Run both commands. Expected: a 0600 secret file, a printed anchor, and a cert record that `sign-tx` displays with the anchor.
- [ ] **Step 3:** Update the docs.
- [ ] **Step 4:** Run the full `ctest`, plus `--suite smoke`. Expected: all pass.
- [ ] **Step 5:** Commit `docs+cli: beacon seed chain`.
