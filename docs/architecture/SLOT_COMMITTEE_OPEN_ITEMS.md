# SlotCommittee — open items

Unsettled product / protocol choices around [`SlotCommittee`](../../src/consensus/SlotCommittee.h)
(equal-weight top‑N committee lottery under beacon authority). Settled rules live in
[WIRE_SCHEMA.md — Leader election](../contracts/WIRE_SCHEMA.md#leader-election-live).

---

## E. Beacon loss → promotion

One beacon at a time. If it is lost or must move, another node takes over, and the old beacon must
not come back as a second authority. **In design:**
[projects/beacon-authority](../../projects/beacon-authority/DESIGN.md) — part 1 makes the beacon
verifiable (admin-signed certificate, signed tips, generations, equivocation halt); part 2 covers
promotion (beacon stolen, server move, everything lost).

## Miner bonds and slashing

Miners register with a fee and a minimum stake, and lose registration after missing due slots
([Miner registration and liveness](../contracts/WIRE_SCHEMA.md#miner-registration-and-liveness)).
There are no bonds and no slashing. Open: whether misbehavior should cost stake, and how it would
be proven on chain.

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
