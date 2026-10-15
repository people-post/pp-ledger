# Beacon authority — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/contracts/` when they ship; mark the
entry **superseded by** that doc then.

## B001 — Authenticate the beacon end to end, anchored in genesis

**Date:** 2026-10-15
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
**Decision:** The recovery set and generation-1 certificate go into genesis as a breaking change.
Existing (test) networks start from a new genesis; no bootstrap path.
**Rationale:** Nothing is released, so no chain needs to survive. A bootstrap path would leave the
admin set on top until it ran.
**Cons:** Every test network and fixture re-genesises; consumers reading genesis or `STATUS`
(pp-browser, pp-node) update in the same release window.

## B009 — Sign every block; keep only the latest signed tip

**Date:** 2026-10-15
**Decision:** The beacon signs every block it commits. The beacon and every node keep only the
latest signed tip (and any equivocation evidence), not a signature per block.
**Rationale:** Signing and verifying ML-DSA cost well under a millisecond — negligible per slot.
Keeping every signature would cost ~3.3 KB per block (~140 MB a day at 2 s slots) for little gain:
the hash chain already ties all history to the latest tip.
**Cons:** No proof later that the beacon signed a specific old block, only that it vouched for a
tip that includes it.
**Alternatives:** Sign every N blocks (fewer signatures, longer unauthenticated window); keep every
signature (offline per-block audit at the storage cost above).
