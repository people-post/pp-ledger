# Domains — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/contracts/` and mark
the entry **superseded by** that doc.

## DN001 — One name: a ticker and a domain

**Date:** 2026-10-19
**Decision:** A system or issuer account's name is both its token's symbol and its domain; it is
NAME_DIRECTORY's `DomainIndex`, given a symbol's form.
**Rationale:** An issuer's token is its account; issuers can be companies issuing shares, so a
ticker is what holders look for, and the same name is where its users and services will live
(`alice@ACME`, DNS). One namespace, one authority (the registrar), one proof.
**Alternatives:** Separate token symbols and domains (two namespaces to police, two lookups);
free-form names, as the Ethereum Name Service has them (squatting, lookalikes).

## DN002 — Letters only; the length tells the kind

**Date:** 2026-10-19
**Decision:** `A`–`Z` only. System accounts 1–3 letters, issued accounts 4–7; user accounts none.
**Rationale:** The owner's design. Short names are scarce and the protocol's: a three-letter symbol
is never an issuer's, which makes lookalikes of system accounts impossible. Letters only rule out
`0`/`O`, `1`/`I`. Real companies with 1–3 letter tickers take a longer one here: this chain is its own
exchange.
**Alternatives:** Any length for issuers (a short symbol no longer means the protocol); digits.

## DN003 — Given once, by genesis or the registrar; read-only

**Date:** 2026-10-19
**Decision:** System names in the genesis block; issued accounts' by the registrar, at creation or
later (`T_DOMAIN_ASSIGN`); never changed, transferred or released in the first release.
**Rationale:** The owner's call: read-only first. The registrar already decides who issues; it now
decides under which symbol, and answers for impersonation (`AAPL` for someone not Apple) off chain.

## DN004 — Ended issuers' domains are retired

**Date:** 2026-10-19
**Decision:** An ended issuer's domain stays bound to it, marked retired, and is never given again.
The registrar's account carries the retired list.
**Rationale:** A symbol reused by a new issuer would let it pass for the old one to holders and
links that remember the symbol. The list lives on an account that is renewed (so a node joining from
a checkpoint has it) and is written in place, as miner state.
**Alternatives:** Released after a quarantine (reuse, later); the list in the genesis meta (chain
state too, but written by a different path than every other account change).

## DN005 — Both directions under the state root

**Date:** 2026-10-19
**Decision:** The domain in the account's leaf, and a domain leaf at `2⁶³ + code(domain)` in the same
tree; account ids below 2⁶³.
**Rationale:** Wallets prove what they show; a symbol is shown to decide where funds go. One tree
keeps one root in the header and one proof shape. A base-27 code of seven letters is below 2³⁴, so
the domain keys never meet account ids once those stay below 2⁶³.
**Alternatives:** A second tree and root (a header change, two proofs); only the account's leaf (no
proof that no one else holds the symbol).

## DN006 — Provisional system names

**Date:** 2026-10-19
**Decision:** The genesis plan names the system accounts; the defaults are `PPL` (genesis, the native
token), `RSV` (reserve), `REG` (registrar), `FEE` (fee), `RCY` (recycle).
**Rationale:** The owner chose to settle them with stakeholders later; a genesis input with defaults
keeps that a configuration choice, not a protocol one.
