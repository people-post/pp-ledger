# Domains — decisions

Rationale for [DESIGN.md](DESIGN.md). When behaviour ships, promote it into `docs/contracts/` and mark
the entry **superseded by** that doc. Built (dn1–dn2): the rules are in WIRE_SCHEMA § Domains.

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
**Superseded by:** [WIRE_SCHEMA.md § Domains](../../docs/contracts/WIRE_SCHEMA.md#domains) (normative; this entry keeps the why).
**Rationale:** The owner's design. Short names are scarce and the protocol's: a three-letter symbol
is never an issuer's, which makes lookalikes of system accounts impossible. Letters only rule out
`0`/`O`, `1`/`I`. Real companies with 1–3 letter tickers take a longer one here: this chain is its own
exchange.
**Alternatives:** Any length for issuers (a short symbol no longer means the protocol); digits.

## DN003 — Given once, by genesis or the registrar; read-only

**Date:** 2026-10-19
**Decision:** System names in the genesis block; issued accounts' by the registrar, at creation or
later (`T_DOMAIN_ASSIGN`); never changed or transferred while the account lives.
**Superseded by:** [WIRE_SCHEMA.md § Domains](../../docs/contracts/WIRE_SCHEMA.md#domains) (normative; this entry keeps the why).
**Rationale:** The owner's call: read-only first. The registrar already decides who issues; it now
decides under which symbol, and answers for impersonation (`AAPL` for someone not Apple) off chain.

## DN004 — A domain ends with its account

**Date:** 2026-10-19 (revised the same day: first built as retired for good, kept on the registrar's
account)
**Decision:** When an account ends, its domain is free again. The registrar, the only one to give
domains, decides whether a freed symbol is given again.
**Superseded by:** [WIRE_SCHEMA.md § Domains](../../docs/contracts/WIRE_SCHEMA.md#domains) (normative; this entry keeps the why).
**Rationale:** The owner's call. A domain is the account's, carried by its renewals; an ended account
is renewed no more, so remembering its domain would take state of its own (a retired list on the
registrar's account) for a rule the registrar can keep itself. Reusing ended account ids raises the
same question; both are left for a later decision, together.
**Alternatives:** Retired for good, on the registrar's account (as first built: a dead issuer's symbol
never comes back, at the cost of chain state that only grows); released after a quarantine.

## DN005 — Both directions under the state root

**Date:** 2026-10-19
**Decision:** The domain in the account's leaf, and a domain leaf `(domain, account)` at `2⁶³ +
code(domain)` in the same tree, while the account lives; account ids below 2⁶³.
**Superseded by:** [WIRE_SCHEMA.md § Domains](../../docs/contracts/WIRE_SCHEMA.md#domains) (normative; this entry keeps the why).
**Rationale:** Wallets prove what they show; a symbol is shown to decide where funds go. One tree
keeps one root in the header and one proof shape. A base-27 code of seven letters is below 2³⁴, so
the domain keys never meet account ids once those stay below 2⁶³.
**Alternatives:** A second tree and root (a header change, two proofs); only the account's leaf (no
proof that no one else holds the symbol).

## DN006 — Provisional system names

**Date:** 2026-10-19
**Decision:** The genesis plan names the system accounts; the defaults are `PPL` (genesis, the native
token), `RSV` (reserve), `REG` (registrar), `FEE` (fee), `RCY` (recycle).
**Superseded by:** [WIRE_SCHEMA.md § Domains](../../docs/contracts/WIRE_SCHEMA.md#domains) (normative; this entry keeps the why).
**Rationale:** The owner chose to settle them with stakeholders later; a genesis input with defaults
keeps that a configuration choice, not a protocol one.
