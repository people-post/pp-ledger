# Ongoing projects

Living design and delivery notes for work in progress. [`docs/`](../docs/README.md) says what the
system **is**; a project folder says what we are **changing**, and why.

## Authority

| Question | Source of truth |
|----------|-----------------|
| Shipped behavior, wire, disk | [`docs/`](../docs/README.md) |
| What is done / next for a project | That project's **`CURRENT_STATE.md`** only |
| Why we chose X | That project's **`DECISIONS.md`** |

Do not put phase status in `AGENTS.md`.

## How to use

1. Read the project's **DESIGN.md** and **CURRENT_STATE.md** before coding.
2. Pick work from **PHASES.md** (ordering only); tick it and update **CURRENT_STATE.md** in the PR
   that does it.
3. Record non-obvious choices in **DECISIONS.md** (date + rationale + alternatives).
4. When wire / disk / behavior ships, **promote** it into `docs/contracts/` (or the right tier) in
   the same PR and mark the decision **superseded by** that doc — no second normative copy here.

## Active projects

| Project | Intent |
|---------|--------|
| [beacon-authority](beacon-authority/) | Verifiable beacon (part 1 shipped: [BEACON_AUTHORITY.md](../docs/contracts/BEACON_AUTHORITY.md)) and promotion after loss or a move (part 2) — open item E |
| [development-budget](development-budget/PLAN.md) | Deliverables, phased roadmap, team and budget plan |
| [platform-integration](platform-integration/DESIGN.md) | A ledger role (relay or miner) in pp-node on its mesh runtime; later pp-browser's client and optional miner (pi1 built: `Server::runOnHost`) |
| [ledger-topology](ledger-topology/DESIGN.md) | Planned topology work: reorg repair, upstream cross-checks, range sync, watermarks, re-calibration |
| [account-recovery](account-recovery/DESIGN.md) | A way back for every user and issued account: an opt-in recovery set above the account's keys names new keys, the account frozen until they take over (built in 1.4.0: [WIRE_SCHEMA § Account recovery](../docs/contracts/WIRE_SCHEMA.md#account-recovery)) |
| [domains](domains/DESIGN.md) | Symbols for system (1–3 letters) and issuer accounts (4–7): a token's ticker and the account's domain, given by genesis or the registrar, read-only, proven both ways (built: [WIRE_SCHEMA § Domains](../docs/contracts/WIRE_SCHEMA.md#domains)) |

## Done / archived

| Project | Shipped as |
|---------|-----------|
| [load-resilience](load-resilience/DESIGN.md) | [LOAD_RESILIENCE.md](../docs/architecture/LOAD_RESILIENCE.md) (behaviour under load) and the `burst`, `overload`, `upstream`, `topology`, `scale` suites; open questions in its DESIGN |
