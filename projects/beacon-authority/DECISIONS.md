# Beacon authority — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/contracts/` when they ship; mark the
entry **superseded by** that doc then. Part 1 shipped (ba8): every entry but B003 (moving the
server — operations, part 2's runbook) names the doc that is now normative; read it there, read
the why here.

## B001 — Authenticate the beacon end to end, anchored in genesis

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Trust chain](../../docs/contracts/BEACON_AUTHORITY.md#trust-chain) (normative; this entry keeps the why).
**Decision:** The admin set signs a beacon certificate (generation + beacon public key); the beacon
signs every tip it commits; nodes adopt blocks only up to a signed tip they can verify back to
genesis. Pinned Amp peer ids remain, but only as a convenience.
**Rationale:** Peer-id pinning authenticates one hop, and miners usually dial relays. Blocks carry
no signature, so a fake upstream can serve a valid-looking chain. A signed tip authenticates every
block below it through the hash chain, without changing the block format.
**Alternatives:** Pinned peer ids only (one hop; does not survive promotion). A beacon signature
stored with every block (offline auditability, but a block-format change and ~3.3 KB per block).

## B002 — Only the admin set raises the generation

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Later generations](../../docs/contracts/BEACON_AUTHORITY.md#later-generations--t_beacon_cert-record-18) (normative; this entry keeps the why).
**Decision:** A new generation needs a certificate signed by the admin set. A beacon cannot sign
a handover to a successor.
**Rationale:** A thief holding the beacon key could otherwise hand itself a higher generation and
fence out the real network.
**Alternatives:** Beacon-signed handover for planned moves (convenient, but breaks case 2).

## B003 — Moving the server is plain operations

**Date:** 2026-10-15
**Decision:** With all keys safe, moving the beacon to a new host keeps its key and generation:
stop writes, copy data and key, start the new host, retire the old. Rotating to a new key is
optional, at the operator's choice.
**Rationale:** Nothing in the protocol changes, so nothing in the protocol is needed. Running both
hosts at once is an operator error the equivocation halt (B005) catches.
**Alternatives:** Always rotate (an admin-key session for every routine move).

## B004 — Recovery set: narrow, slow, visible

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Recovery](../../docs/contracts/BEACON_AUTHORITY.md#recovery--t_recovery-record-19) (normative; this entry keeps the why).
**Updated:** 2026-10-15 — delay and admin freeze in B007.
**Decision:** Genesis carries a recovery key set with its own threshold. Its **only** power is to
replace the admin set. A recovery action is announced on chain and takes effect only after a
delay; the admin set cannot block it. The recovery set changes only with its own signatures and
cannot be changed by `T_CONFIG`.
**Rationale:** Some key set has to be on top; the safest one is the set used least. The delay and
the announcement give people time to notice and step in (out of band) before a recovery takes
effect. Letting the admin set veto would let stolen admin keys defeat the recovery.
**Alternatives:** A stronger admin threshold only (e.g. 3 of 5 held apart) — simpler and no format
change, but losing a majority of admin keys at once means re-genesis. Admin-set veto (rejected:
defeats recovery from admin theft).

## B005 — Equivocation halts nodes

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Equivocation halt](../../docs/contracts/BEACON_AUTHORITY.md#equivocation-halt) (normative; this entry keeps the why).
**Decision:** Two different signed tips at the same generation and height halt block commits and
production above that height on any node that sees them; nodes keep serving reads, alert, pass the
evidence on, and resume on a valid higher-generation certificate. The beacon signs only after a
durable commit and persists its last signed height, so an honest beacon cannot equivocate.
**Rationale:** Equivocation means a stolen beacon key or two hosts running it; either way following
it splits the network for good (there is no fork choice). Producing the evidence takes the beacon
key, so halting adds no new denial of service.
**Alternatives:** Alert only (nodes keep following a possibly stolen beacon).

## B006 — A separate beacon signing key

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Genesis authority](../../docs/contracts/BEACON_AUTHORITY.md#genesis-authority), [LEDGER_STORAGE.md § Node state files](../../docs/contracts/LEDGER_STORAGE.md#node-state-files) (normative; this entry keeps the why).
**Decision:** The beacon signs tips with its own ML-DSA key, `keys/beacon-signing.key`, generated at
`pp-beacon --init`; the generation-1 certificate is signed there by the genesis keys. The Amp
identity key stays transport-only. The certificate binds the signing key; it may carry the current
Amp peer id as an unsigned dial hint.
**Rationale:** Each key rotates alone: a new Amp identity is an operations change, a new signing key
is a new generation. Chain authority does not depend on transport details.
**Cons:** Two keys to protect and back up. No help against theft — both live on the beacon host.
**Alternatives:** Reuse the Amp identity key (one key, but rotating transport would need an admin
session).

## B007 — Recovery delay about a month; admin set frozen meanwhile

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Recovery](../../docs/contracts/BEACON_AUTHORITY.md#recovery--t_recovery-record-19) (normative; this entry keeps the why).
**Decision:** A recovery action applies a fixed number of epochs after it is announced (~30 days of
slots; set in genesis). While it is pending, the admin set can issue no beacon generation and no
`T_CONFIG`. The pending recovery is shown on `STATUS`, in logs and as an operator alert.
**Rationale:** A month gives people time to notice and act. Freezing the admin set stops stolen
admin keys from using that month; counting in epochs makes every node apply it at the same block.
**Cons:** Lost admin keys plus a lost beacon can stall the chain for up to the delay (no new
generation until the recovery applies). If the recovery set is the stolen one, honest admins are
frozen for the month too (they could not block it anyway).
**Alternatives:** Shorter delay (less time to react); no freeze (stolen admin keys keep full power
for the month).

## B008 — Breaking genesis change, no migration

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Formats](../../docs/contracts/BEACON_AUTHORITY.md#formats) (normative; this entry keeps the why).
**Decision:** The recovery set and generation-1 certificate go into genesis as a breaking change.
Existing (test) networks start from a new genesis; no bootstrap path.
**Rationale:** Nothing is released, so no chain needs to survive. A bootstrap path would leave the
admin set on top until it ran.
**Cons:** Every test network and fixture re-genesises; consumers reading genesis or `STATUS`
(pp-browser, pp-node) update in the same release window.

## B009 — Sign every block; keep only the latest signed tip

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Signed tip](../../docs/contracts/BEACON_AUTHORITY.md#signed-tip) (normative; this entry keeps the why).
**Decision:** The beacon signs every block it commits. The beacon and every node keep only the
latest signed tip (and any equivocation evidence), not a signature per block.
**Rationale:** Signing and verifying ML-DSA cost well under a millisecond — negligible per slot.
Keeping every signature would cost ~3.3 KB per block (~140 MB a day at 2 s slots) for little gain:
the hash chain already ties all history to the latest tip.
**Cons:** No proof later that the beacon signed a specific old block, only that it vouched for a
tip that includes it.
**Alternatives:** Sign every N blocks (fewer signatures, longer unauthenticated window); keep every
signature (offline per-block audit at the storage cost above).

## B010 — The authority rides the genesis account meta

**Date:** 2026-10-15
**Superseded by:** [WIRE_SCHEMA.md § Config updates](../../docs/contracts/WIRE_SCHEMA.md#config-updates), [BEACON_AUTHORITY.md § Formats](../../docs/contracts/BEACON_AUTHORITY.md#formats) (normative; this entry keeps the why).
**Decision:** The recovery set and the current beacon certificate (`ChainAuthority`) live in
`GenesisAccountMeta` (version 8) beside the config. `T_GENESIS` sets them — so the admin keys that
sign genesis sign the generation-1 certificate; `T_CONFIG` and genesis renewals must carry them
unchanged. The certificate has no `issuedAt` (the committing block dates it). The beacon signing key
is a file pair, `keys/beacon-signing.key` / `.pub` (the format `pp-client keygen -o` writes):
`--init` creates it when absent, reuses it when both exist and match. `recovery.delayEpochs`
defaults to ~30 days of epochs.
**Rationale:** Genesis renewals are what a node joining from a checkpoint reads, so the authority
reaches it the same way the config does, with no new record or sync path. A file pair lets an
operator generate the key off the beacon first; ML-DSA private keys here cannot yield their public
key.
**Alternatives:** Fields in `BlockChainConfig` (would make the authority look like config that
`T_CONFIG` may change); a separate genesis record for the certificate (deferred to ba2's
`T_BEACON_CERT` for later generations).

## B011 — `T_BEACON_CERT`: a record of its own, one per block, in force at once

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Later generations](../../docs/contracts/BEACON_AUTHORITY.md#later-generations--t_beacon_cert-record-18) (normative; this entry keeps the why).
**Decision:** Later generations ride a new record type, `T_BEACON_CERT` (18), signed by the admin
set (genesis account), fee `0`, whose `meta` is the `BeaconCertificate` (versioned). On chain it
must be exactly the next generation, with a fresh key (not the current beacon, admin or recovery
key) and a `cutoverHeight` that does not go back and lies below the committing block. It is in
force as soon as its block applies (not at an epoch boundary), and a block holds at most one: the
draft refuses a second (`AccountBuffer` draft tags), since drafts check against the committed
authority and two would both pass there yet seal a block the chain refuses. Out of band,
`Chain::verifyBeaconCertificateRecord` accepts any higher generation the admin set signed. The
tooling is the existing offline flow: `pp-client mk-beacon-cert` writes the unsigned record,
admins `sign-tx --network-id`, anyone `submit-tx`.
**Rationale:** A takeover (case 2) wants the new beacon recognised in the next block, not an epoch
later; the cutover height, not the record's block, bounds what the old generation may sign. A
dedicated record keeps the authority off the config path (`T_CONFIG` still cannot touch it, B010)
and gives the admin set a narrow, auditable action. Skipping generations out of band lets a node
that missed a block still follow the newest beacon (ba5); on chain, exact succession keeps the
history gap-free.
**Cons:** A leaked admin quorum can certify a beacon at once — the same trust as `T_CONFIG`
replacing the admin keys at once; the recovery set (ba7) is the remedy.
**Alternatives:** Carry the certificate in `T_CONFIG` (would apply at the epoch boundary and blur
config with authority); several certificates per block applied in order (needs per-draft authority
state for one rare record).

## B012 — Signed tip: one file, written before it is served; relays keep the newest

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Signed tip](../../docs/contracts/BEACON_AUTHORITY.md#signed-tip), [LEDGER_STORAGE.md § Node state files](../../docs/contracts/LEDGER_STORAGE.md#node-state-files) (normative; this entry keeps the why).
**Decision:** The beacon signs each block right after `Chain::addBlock` commits it, under the
certificate in force after that block, and writes the tip to `data/signed-tip.dat` (versioned;
write aside, then rename) before any reply can carry it. The file is the "last signed height":
signing a different block at or below it is refused, and at start a ledger that ends below it or
holds another block at its height is refused (restore the ledger). A start signs the committed tip
if it is unsigned, which covers genesis and a crash between commit and signing. A beacon whose key
the certificate in force does not name keeps running and signs nothing. The tip travels as a
packed field on `STATUS` and in a new `TipReply {nextBlockId, signedTip?}` that replaces the
`BLOCK_WAIT` and `BLOCK_ADD` reply payloads (breaking, B008). Relays hold the newest tip they hear
on any of `STATUS` / `BLOCK_WAIT` / `BLOCK_ADD` / `REGISTER` (higher height, or higher generation
at one height) and serve it unverified; a second tip for the same generation and height is
ignored with a warning. The current certificate on `STATUS` moves to ba5, which needs it.
**Rationale:** Writing before serving is what makes "never two tips at one height" survive a
crash: a signature that was never stored was never sent. Signing in the commit path, not on a
timer, gives every block its own tip with no extra state. A relay serving what it heard, unchecked,
keeps ba3 to transport; verification belongs with the adoption rule (ba4), where a node also has
to decide what to do with a bad tip.
**Cons:** A relay can serve a tip ahead of the blocks it holds (it heard the tip first); downstreams
sync to it. Until ba4 a relay passes on a forged tip as readily as a real one.
**Alternatives:** Persist only the height and re-sign on start (a restart would produce a second,
different signature for the same tip, which ba6 could mistake for equivocation unless signatures
are deterministic); verify at the relay now (needs the generation rules of ba5 to be complete).

## B013 — Adoption: authenticate the range, then apply; first block on trust

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Adoption rule](../../docs/contracts/BEACON_AUTHORITY.md#adoption-rule) (normative; this entry keeps the why).
**Decision:** `BlockSync` takes the upstream's signed tip from `STATUS` (replacing `CALIBRATION`),
verifies it against the generation in force locally, then works in two passes. *Authenticate:*
fetch the blocks from the local tip to the signed height and check that each one's computed hash
is the next link, ending at the signed hash; nothing is committed. *Apply:* commit them in order,
each matching its authenticated hash. Up to 1024 bodies are kept between the passes; beyond that
only hashes (32 bytes a block) and the bodies are fetched again. Blocks above the signed tip wait
for the next tip. A miner commits its own block only when a `BLOCK_ADD` reply carries a verified
tip for exactly that block; a relay stores a forwarded block under the same rule; relays keep only
tips that verify. A node with no blocks takes its first block (genesis, or a miner's checkpoint
renewal) on trust, because that block is where it learns the beacon's key.
**Rationale:** One signature authenticates a whole range only through the hash chain from its top,
so a range can be trusted only once its last block is seen; committing as blocks arrive would let a
fake upstream plant blocks below a tip it cannot forge. Two passes bound memory on a long
catch-up without a header-only RPC. Committing only on a covering tip closes the old gap where a
miner committed its block on any upstream success, even one that never reached the beacon.
**Cons:** Near the head, a block commits a little later (after the whole range arrives, not as each
does). A catch-up beyond 1024 blocks downloads most bodies twice. The first block is a trust root
taken from the upstream: a miner can pin genesis (`networkAnchor.genesisHash`), but nothing ties a
checkpoint start to it yet — to close with ba5, which already has to carry certificates forward
from genesis.
**Alternatives:** Apply as blocks arrive and roll back on a bad tip (needs rewind, part 2);
fetch backwards from the tip (same memory problem, more complex); verify a checkpoint start from
the pinned genesis now (needs the generation rules of ba5 when the key changed since genesis).

## B014 — Generations follow the admin-signed trail; rotation by a staged key

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Adoption rule](../../docs/contracts/BEACON_AUTHORITY.md#adoption-rule), [§ Signed tip](../../docs/contracts/BEACON_AUTHORITY.md#signed-tip) (normative; this entry keeps the why).
**Decision:** A node checks a signed tip against the generation in force *at the tip's height*,
found by following the blocks from its own state (`AuthorityTrail`): a `T_CONFIG` replaces the
admin set and a `T_BEACON_CERT` raises the generation, each only if the admin set in force there
signed it. `BlockSync` checks a tip of the current generation at once and one of a later
generation after following the range; a covering `BLOCK_ADD` tip at a relay is checked over the
block it covers (a miner already applied its sealed block). A tip of an older generation is
superseded. The beacon holds `keys/beacon-signing.key` and an optional staged
`keys/beacon-signing.next.key`, and signs with whichever the certificate in force names: that is
a same-server rotation. If it holds neither, it stands down: it refuses `BLOCK_ADD`, `TX_ADD`,
`TX_PULL` and `REGISTER` and serves reads. Miners choose the upstream with the highest tip
generation, then height. A miner that pins genesis also pins its first block: the checkpoint when
it carries the genesis authority, else genesis (and syncs from it). Certificates that are not on
chain yet (STATUS, any peer, a file; the highest seen persisted; `cutoverHeight` against them) are
split into ba5b.
**Rationale:** On chain, every certificate arrives inside the blocks being synced, so following
them needs no new wire field or stored state, and admin signatures root the trail in what the
node already trusts — an upstream cannot move it. Following `T_CONFIG` too keeps a node that slept
through an admin rotation able to verify a later certificate. A staged key gives a rotation with
no gap in signing and no restart at the switch, and makes generations testable end to end
(`--suite rotation`). "Runs as a relay" needs the new beacon's address, which is part 2
(candidate lists), so for now standing down means read-only. Out-of-band certificates matter only
when the old beacon will not commit the new one (stolen, case 2), which part 2 handles with
rewind; ba5b prepares the channel.
**Cons:** Until ba5b a stolen beacon key keeps signing for nodes the admins cannot reach on chain.
`cutoverHeight` is enforced only implicitly (generation N signs nothing at or above the block
that certifies N+1). (B017 makes it explicit: a certificate commits by the block after its cutover.) Relays and unanchored miners still take their first block on trust. A
certificate for a key no running beacon holds halts the chain until that beacon runs (part 2).
**Alternatives:** The current certificate on `STATUS`, checked against the node's admin set
(fails after an admin rotation the node has not applied; kept for ba5b's out-of-band channel);
rename the next key over the current one at the switch (file moves in the commit path; left to
the operator at a restart).

## B015 — A checkpoint is where a miner can start: an epoch start, self-describing

**Date:** 2026-10-15
**Superseded by:** [BLOCK_PIPELINE.md § Checkpoints and late join](../../docs/architecture/BLOCK_PIPELINE.md#checkpoints-and-late-join), [WIRE_SCHEMA.md § Account / ledger model](../../docs/contracts/WIRE_SCHEMA.md#account--ledger-model) (normative; this entry keeps the why).
**Decision:** A checkpoint is the first block of an epoch, at least `checkpoint.minBlocks` after the
current checkpoint, whose previous block's slot starts more than `checkpoint.minAgeSeconds` after
the current checkpoint's block (`chain_block::rotatesCheckpoint`). It opens with the genesis
renewal (due or not), carrying config, pending update, admin set and authority. Every epoch's first
block carries its committee, the preimage of `stakeSnapshotHash` (ba5b.3). A miner starts at the
upstream's previous checkpoint (`lastId`): it installs what the genesis renewal carries, replays
blindly while every account renews (all are due by the block before the current checkpoint), and
from the current checkpoint checks every block and may seal. Renewal due-sets and rotation read
the block's slot time and stored blocks, never a node's clock.
**Rationale:** A checkpoint is defined as the point a miner can join and start working, so the
checkpoint itself must hold what a joining node lacks: config and authority (the genesis renewal),
and the committee each epoch elects leaders from (liveness charges for skipped slots depend on it,
and a node that holds only some accounts cannot compute it). Patching joiners instead — warm-up
without config, recording liveness charges in every block — would fix symptoms of a checkpoint that
was not a starting point. The old rules also read the wall clock: with the 1-year default
`checkpointMinAgeSeconds`, nodes disagreed on due renewals and a replay could refuse history.
**Cons:** Breaking chain-format and rule changes (no release yet). Checkpoints rotate up to an epoch
later than before; the genesis account renews up to twice as often. Warm-up state is still taken on
trust (the beacon vouches through its signed tip), not re-validated.
**Alternatives:** Start from older checkpoints (does not converge: an inexact committee keeps
producing wrong charges); a self-checking Full gate on a matching `stateRoot` (may never come while
slots are skipped); liveness charges listed in each block (works, but fixes a symptom and costs
every block); the whole state in the checkpoint block (unbounded size).

## B016 — Warm-up is exact, or it stops

**Date:** 2026-10-15
**Superseded by:** [BLOCK_PIPELINE.md § Checkpoints and late join](../../docs/architecture/BLOCK_PIPELINE.md#checkpoints-and-late-join) (normative; this entry keeps the why).
**Decision:** A node warming up from a checkpoint (CheckpointReplay) applies every effect a record
has on the accounts it holds, whoever else the record touches: payer debit, payee credit and fee
credit each land if that account is held (`chain_tx::applyTransferToHeldAccounts`); a new
account is created whether or not its payer is held; a miner record's fee reaches a held fee
account; the checkpoint block's liveness charges start at its epoch's first slot. `T_END_USER`
carries the balances it writes off (Full checks them), so a node that does not hold the ended
account still credits the recycle account. Renewals of held accounts keep their balance and
miner-state checks while warming up, and the first Full block checks `stateRoot` and the stake
snapshot: a diverged warm-up stops the node with an error. A node started at a checkpoint seals
only once its blocks reach back over the transaction validity window (`Chain::sealReadiness`).
**Rationale:** The rotation smoke showed the old soft paths skipping effects (fees of transfers
between accounts not yet held never reached the held fee account); an audit of every handler found
the rest. Recording what a warming node cannot derive (the written-off balances, like the epoch
committee in B015) keeps the warm-up a pure replay of block data. Idempotency is checked against
held blocks, so sealing before the history covers the validity window could include a repeated
transaction — the smoke's freshly joined miner did, and every block it sealed was refused.
**Cons:** With a chain younger than the validity window (one day by default) a joined miner
validates but does not seal; with the defaults (checkpoints a year apart) it never waits.
**Alternatives:** Renewals overwrite a held account without checking (self-heals a divergence but
hides bugs); a rotation rule tied to the validity window (misses a later `T_CONFIG` raising it).
Also fixed here: a transfer to the payer itself minted its amount (`transferBalance` read both
balances before writing) — refused now.

## B017 — Certificates out of band bound the old generation; the cutover bounds the commit

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Certificates out of band](../../docs/contracts/BEACON_AUTHORITY.md#certificates-out-of-band), [§ Later generations](../../docs/contracts/BEACON_AUTHORITY.md#later-generations--t_beacon_cert-record-18) (normative; this entry keeps the why).
**Decision:** Every node keeps the highest admin-signed `T_BEACON_CERT` it sees outside the blocks
(`Chain::offerCertificate`, verified against its committed admin set and generation), from
`STATUS` (`certificate`: the one held, else the latest on chain), a `TX_ADD` passing through it
(relays and miners keep it and forward it; the beacon keeps it and pools it), or an operator's
`submit-tx` to the node; it is written to `data/beacon-cert.dat` before the offer returns and
offered again at start (a node with no block yet waits for its first). Until a block commits that
generation, a tip of an older generation above its cutover does not verify (`AuthorityTrail`);
`BlockSync` offers the upstream's certificate before checking its tip, a relay stops serving a
tip it rules out, a miner skips such an upstream. A beacon holding no key it names stands down; a
beacon holding it commits no block above the cutover that does not commit a certificate. It is
dropped once committed (or a later one is), or when an admin rotation leaves it unsigned. On chain
the cutover rule changes: a certificate commits **by the block after its cutover** (was: the
cutover below the commit block); the certificate in force carries the block before its commit as
its cutover; `2^64−1` (`mk-beacon-cert` without `--cutover`) means none. A tip of the new
generation still verifies only along blocks that commit it.
**Rationale:** Out of band is what a stolen key needs (case 2): its holder will never commit its
successor, so nodes must refuse its tips on the admins' word alone, and only a self-verifying
certificate carries that word. Checking it against the node's own admin set keeps an upstream from
planting one. The old on-chain rule allowed `cutover < commit block`, so the old generation could
sign blocks above its cutover that then stayed on chain: once nodes refuse those tips out of band,
an honest rotation with a cutover below its commit block deadlocks (nodes refuse the blocks the
certificate must be committed above). Bounding the commit by the cutover makes the two agree; the
in-force cutover keeps "cutover does not go back" meaningful after a generous one. An honest
rotation needs no bound at all, hence "none". Following the new generation before its certificate
is on chain would let a node accept blocks the new beacon has not yet committed; that, and
rewinding nodes past the cutover, is part 2.
**Cons:** Nodes holding blocks above a cutover stop following and need part 2's rewind. A cutover
the chain passes before the certificate commits makes it uncommittable (the staged beacon stops at
the cutover instead of passing it; a new certificate is needed). Another admin-signed certificate
for the generation held is refused (the first stays) — the admins issue a higher generation
instead. A node with no blocks syncs its first range under the authority of its first block only.
**Alternatives:** Keep `cutover < commit` and enforce cutovers only once on chain (no protection
before the commit: useless against a stolen key); accept the new generation's tips out of band
(lets a node follow a branch no block certifies); refuse certificates in `TX_ADD` and use only
`STATUS` and a file (a cut-off node's operator then needs a new tool; `submit-tx` to the node
already reaches every role).

## B018 — Equivocation: evidence by block hash, halt at the tip gate, resume on a later generation

**Date:** 2026-10-15
**Superseded by:** [BEACON_AUTHORITY.md § Equivocation halt](../../docs/contracts/BEACON_AUTHORITY.md#equivocation-halt) (normative; this entry keeps the why).
**Decision:** Evidence is two signed tips of one network, generation and height naming different
blocks (`Client::Equivocation`), checked against that generation's key — in force, or held out of
band. Equal block hashes with different signatures are no evidence. A node keeps one piece per
generation (`Chain::offerEquivocation`, persisted in `data/equivocation.dat`) and halts by refusing
that generation's tips above the height in `AuthorityTrail::verifyTip`; the beacon refuses commits
above it and producer writes, taking only a beacon certificate. Any later certificate, out of band
or on chain, supersedes the generation and resumes. Evidence travels down on `STATUS`
(`equivocation`) and up by `EQUIVOCATION_ADD`; relays build it from two upstream tips at one height,
miners from tips of different upstreams, and push it up (miners to every upstream).
`pp-client mk-equivocation` makes drill evidence from a beacon key.
**Rationale:** ML-DSA signatures are randomized, so a re-signed block would look like equivocation
by signature. Every adoption path already ends in `verifyTip`, so one gate halts sync, own-block
commits and relays alike. A certificate is the only resume B005 allows; any later generation means
the admins acted. Pushing up reaches the beacon (it must halt too); each side of a two-host split
learns through the miners that see both.
**Cons:** Only the latest tip is kept (B009), so a conflicting tip below a node's latest goes
unnoticed; evidence for a generation the node cannot check yet is refused until its certificate
arrives. The beacon accepts evidence against its own key from any downstream — producing it takes
that key, so this adds no denial of service.
**Alternatives:** Compare signatures (false positives from randomized signing); halt every write
including certificates (no way out without an operator); keep every tip seen for detection
(storage B009 avoided).

## B019 — Recovery: a record of its own, pending in the authority, applied in place at its epoch

**Date:** 2026-10-16
**Superseded by:** [BEACON_AUTHORITY.md § Recovery](../../docs/contracts/BEACON_AUTHORITY.md#recovery--t_recovery-record-19) (normative; this entry keeps the why).
**Decision:** The recovery set announces a new admin set with `T_RECOVERY` (record 19), signed by
the recovery set in the chain authority rather than by an account. Committed, it sits in
`ChainAuthority` as pending (`GenesisAccountMeta` v9), so genesis renewals and checkpoints carry it.
`applyEpoch` is the announcing block's epoch plus `delayEpochs`. The first block at or after that
epoch replaces the genesis account's keys and threshold in place, before its records. One recovery
is pending at a time; the admin set cannot cancel it. While it is pending, `T_CONFIG` and
`T_BEACON_CERT` are refused, in blocks of earlier epochs and out of band. A certificate held out of
band is checked again when the announcement commits and when it applies. `AuthorityTrail` follows
the same rules, and each `AuthorityStep` carries its block's epoch so proof replays apply the
recovery where the chain did. Visibility: `STATUS.recovery`, `pp-client status`, and error-level
alerts at the announcement, each epoch while pending, and the apply.
On the way, two fixes:
- A genesis renewal must carry the admin set in force. The slot leader signs renewals and could
  otherwise slip its own keys in as the admin set.
- Renewals are built and checked as the block's epoch holds the authority, so the apply epoch's
  renewal carries the new set.
**Rationale:** The recovery set is no account, so its signature check is the authority's own. In
the authority, the pending recovery reaches checkpoint starters with no new sync path (B010).
Applying at an epoch's first block makes every node apply it at the same block; in place keeps
renewal's meta lookup where it was. The freeze is B007's: generations and config.
**Cons:** Issuance by the admin set is not frozen (B007 names generations and config only), so
stolen admin keys can still mint up to `maxIssuancePerEpoch` per epoch while a recovery waits. A
lost beacon plus lost admin keys stalls the chain until the recovery applies. The recovery set
cannot change itself yet. One recovery at a time: a mistaken announcement waits out its delay.
**Alternatives:** An explicit "effect" record at the apply epoch (an extra record nobody needs to
sign); keep the pending recovery outside the authority (checkpoint starters would miss it);
replace the genesis account through `replaceGenesisAccount` (moves its blockId, breaking renewal
when the apply block holds no genesis meta).

## B020 — The authority is a contract of its own; one certificate per block is a block rule

**Date:** 2026-10-16
**Superseded by:** [BEACON_AUTHORITY.md](../../docs/contracts/BEACON_AUTHORITY.md), [LEDGER_STORAGE.md § Node state files](../../docs/contracts/LEDGER_STORAGE.md#node-state-files) (normative; this entry keeps the why).
**Decision:** The beacon authority's normative text — formats and versions, records 18 / 19, signed
tip, adoption rule, certificates out of band, equivocation halt, authority proof, RPC fields — is
one contract, `docs/contracts/BEACON_AUTHORITY.md`; WIRE_SCHEMA keeps record ids and the genesis
meta and points there. Node state files are part of the disk contract (`LEDGER_STORAGE.md`). "At
most one `T_BEACON_CERT` (and one `T_RECOVERY`) per block" becomes a structural block rule checked
in every admission mode (`chain_block::validateAuthorityRecordCount`) and by `AuthorityTrail`, not
only a producer's draft check; a node's certificate file is removed as soon as the chain drops the
certificate.
**Rationale:** The authority grew phase by phase inside a document about block layouts; protocol
behaviour (who refuses what, when) does not belong there, and one file per concern is easier to keep
true. Checking every claim against the code found the draft-only rule: a block certifying N+1 and
N+2 passed each certificate's own checks in turn, so validators accepted a generation jump the
contract forbids.
**Cons:** A block holding two certificates was valid before and is not now (breaking; nothing is
released, B008). The state files are not synced to the device: a power loss can tear one (the node
then refuses to start) — durability is left open (closed by B021).
**Alternatives:** Keep the authority in WIRE_SCHEMA with sub-headings (one long file mixing layout
and protocol); fix the per-block rule by checking "exactly next generation" against the trail
(already true per record — the gap is two records in one block).

## B021 — Durable commits: the block-file header is the commit point; the beacon commits every block

**Date:** 2026-10-16
**Superseded by:** [LEDGER_STORAGE.md § Durability](../../docs/contracts/LEDGER_STORAGE.md#durability) (normative; this entry keeps the why).
**Decision:** Nothing was synced to the device: streams were flushed to the OS only. Now a block
file's header count is the commit point — appends write data; `FileStore::commit` syncs it, writes
the count, syncs again — and mount keeps the committed blocks and truncates the rest. The beacon
commits every block before it signs it (`Ledger::CommitPolicy::everyBlock`); relays and miners
commit every 1000 blocks or 5 s, and on shutdown. The three indexes are written only when the
structure they list changes, durably (aside, sync, rename, directory sync), each new block file or
volume synced before an index names it; an index naming a missing block file is refused. The node
state files are replaced the same way (`utl::replaceFile`).
**Rationale:** "Never two tips at one height" (B009, B012) assumes the signed block and the stored
tip survive whatever stops the beacon. After a power loss an unsynced tip file could come back
older than a tip already served, and an unsynced block could be gone while its tip was served —
the beacon would then sign a second block at a served height, which every node takes as
equivocation. Syncing the block before signing and the tip before serving closes that. Making the
header count the commit point gives mount a boundary to trust: before, the count was rewritten on
every append in the page cache, and the FileDirStore index was rewritten in place on every append —
a crash mid-write left an index that would not mount, and a block written without its count was
read back at the wrong id after the next append.
**Cons:** Each beacon block costs two data syncs plus the tip file's sync and rename (milliseconds on
an SSD, per block, not per transaction). A relay or miner crash loses its uncommitted tail (it syncs
it again). Two processes must not mount one ledger directory (a mount trims the other's tail).
**Alternatives:** Sync every block on every node (a relay's catch-up of a long chain would pay one
sync per block); checksums per block to find a torn tail (a format change, and the header count
already bounds what was committed); trust a scan of the file (zero-filled pages parse as empty
blocks).

## B022 — A rewind is a restart

**Date:** 2026-10-16
**Decision:** A node rewinds by restarting, never in place: it writes a durable marker
(`data/rewind.dat`: the target height and the certificate ordering it) and stops; the app's `main()`
builds a fresh role server on the same work directory, which applies the marker before anything
mounts the ledger — truncate to the cutover, or discard and join fresh when the cutover lies below
the node's starting checkpoint — and then starts as from cold. One rewind per certificate
generation; a marker that survives its restart stops the node with an error.
**Rationale:** Part 2's events are rare — perhaps once in a network's life — so maintainability
beats speed (the user's call: slow is fine, even a reboot). A restart rebuilds chain state,
committee, idempotency, pools, sync and network from the ledger through paths every node runs
daily; an in-place rewind needs undo for each of them, code that would run once a year and break
unnoticed. Applying the marker before mount means no live object holds a file and no in-memory
state is half-updated; the ledger's commit point (B021) makes each step safe to repeat after a
crash.
**Cons:** A rewind costs a full reload (the beacon and relays replay from genesis, as at every
start). The node is unavailable for the restart.
**Alternatives:** Undo records per block (fast, much new state); rewind in place and reload the chain
object only (other objects keep stale state); exit and rely on a supervisor (third-party nodes may
have none — B023).

## B023 — Every node adopts a cutover on its own

**Date:** 2026-10-16
**Decision:** A node that keeps a certificate whose cutover is below its committed tip rewinds
without an operator: the restart of B022 happens inside the process (a loop in `main()` around a
fresh server), so no service supervisor is needed. Only an admin-signed certificate of a higher
generation, verified against the node's own admin set, can order it. The node alerts and reports
the rewind (log, `STATUS`).
**Rationale:** Many miners and relays are not operated by the chain's maintainer; a case-2 recovery
that waits for each of them to act would leave the network split for as long as the slowest
operator. The authority to order a rewind already exists — the admin set signs the cutover (B017) —
so the node needs no further trust to act on it, and a stolen beacon key or a lying upstream cannot
trigger one.
**Cons:** Clients see history above the cutover disappear and must resubmit; a mistaken cutover
from the admins rewinds the whole network.
**Alternatives:** Operator-driven rewind (slow, partial); exit for a supervisor to restart (not every
node has one).

