# SlotCommittee — open items

Unsettled product / protocol choices around [`SlotCommittee`](../../include/ledger/consensus/SlotCommittee.h)
(equal-weight top‑N committee lottery under beacon authority). Settled rules live in
[WIRE_SCHEMA.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live).

---

## E. Beacon loss → promotion

One beacon at a time. If it is lost or must move, another node takes over, and the old beacon must
not come back as a second authority. Part 1 — a verifiable beacon (admin-signed certificate, signed
tips, generations, equivocation halt, recovery set) — has shipped:
[BEACON_AUTHORITY.md](../contracts/BEACON_AUTHORITY.md). **In design:** part 2, promotion (beacon
stolen, server move, everything lost) —
[projects/beacon-authority](../../projects/beacon-authority/DESIGN.md).

## Seed grinding — closed

The seed of epoch `E` used to hash the last block hashes of `E-1`, so the leader of `E-1`'s last
block could try block variants until the seed made it leader of `E`'s last slot, and repeat
indefinitely. Closed by the beacon seed chain: the seed mixes a reveal from the beacon's committed
hash chain (`T_BEACON_SEED`) and no block content, and `E`'s committee is fixed one epoch ahead, so
nothing a producer does after the reveal is public can reorder it
([Epoch seed](../contracts/WIRE_SCHEMA.md#epoch-seed),
[BEACON_AUTHORITY.md § Seed chain](../contracts/BEACON_AUTHORITY.md#seed-chain)). Residuals:

- The beacon can withhold a reveal (the epoch gets the absent seed), not choose one.
- A producer has an include/omit bit, and only in a narrow case: when every earlier block of
  `E-1` left the pooled reveal out, the leader of `E-1`'s last block chooses between the reveal
  seed and the absent seed for `E`. That is at most one bit per epoch, never a choice of value.
  Honest leaders include a pooled reveal even in renewals-only blocks (the leader keeps it a place
  ahead of waiting renewals), so the case needs every leader of `E-1` to have left it out.

## Miner bonds and slashing

Miners register with a fee and a minimum stake, and lose registration after missing due slots
([Miner registration and liveness](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness)).
There are no bonds and no slashing. Open: whether misbehavior should cost stake, and how it would
be proven on chain. With [producer keys](#miner-producer-keys), conflicting blocks signed by a stolen
producer key are the case it would have to cover.

## Miner producer keys

A miner signs with its **account's own keys**: each block it leads (`producerSignatures`,
[Block](../contracts/WIRE_SCHEMA.md#block)) and its sign-in to its upstream
([Miner sign-in](../contracts/WIRE_SCHEMA.md#miner-sign-in-register-request)). So the host that runs
`pp-miner` holds the keys that also move the account's stake and balance and sign its
registration: a compromised miner host loses the account.

Proposed, as the beacon's key works ([BEACON_AUTHORITY.md](../contracts/BEACON_AUTHORITY.md)): the
miner account, its keys offline (M-of-N), certifies a **producer key** on chain, a record signed
by the account naming the key's public key and a sequence. Blocks and sign-ins are checked against
the producer key in force for the slot; the producer key signs nothing else, so it cannot move
funds, change the account or register it. A lost or stolen producer key is replaced by a new
certificate from the account, without touching the account's keys. What a thief can still do is
sign conflicting blocks in the slots the account leads: the misbehaviour
[Miner bonds and slashing](#miner-bonds-and-slashing) would make cost stake.

Open: the record (its own type or a field of `T_MINER_REGISTER`), whether a certificate expires,
when a new producer key takes effect (the next epoch, as the committee is fixed ahead), and
whether an account without one keeps signing with its own keys. Whatever the answer, it changes
what every node verifies, so it is decided and built before the first release freezes the chain
format ([projects/release PHASES § R5](../../projects/release/PHASES.md#r5--200)); after that it
needs rule activation ([§ R3](../../projects/release/PHASES.md#r3--rule-activation)).

## Private leader selection

An epoch's leaders are knowable once its seed is public
([Epoch seed](../contracts/WIRE_SCHEMA.md#epoch-seed)), so the schedule can be used to target the
next leader. Open: whether to add private leader proofs (VRF-style), and at what cost.

---

## Related docs

- [WIRE_SCHEMA.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live)
- [DESIGN.md](../product/DESIGN.md) — beacon / relay / miner roles
- [LEDGER_TOPOLOGY.md](LEDGER_TOPOLOGY.md)
- [src/consensus/README.md](../../src/consensus/README.md)
