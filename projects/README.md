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
| [beacon-authority](beacon-authority/) | Verifiable beacon (certificate, signed tips, generations) and promotion after loss or a move — open item E |

## Done / archived

_None yet._
