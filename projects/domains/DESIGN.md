# Domains — symbols for system and issuer accounts

**Status:** design (dn0). Phases: [PHASES.md](PHASES.md) · decisions: [DECISIONS.md](DECISIONS.md) ·
status: [CURRENT_STATE.md](CURRENT_STATE.md).

Accounts are numbers today: a token is "token 1,048,626", and a wallet cannot tell a real issuer from
a lookalike number. This project gives **system and issuer accounts a domain**: a short symbol of
capital letters that is at once the account's **ticker** — its token's symbol, as a stock exchange
lists shares — and its **domain**, as a top-level domain names a zone. It is the `DomainIndex` of
[NAME_DIRECTORY](../../docs/product/NAME_DIRECTORY.md) (domains owned by reserved accounts), built
first, with the symbol as the domain's form (DN001).

## The names (DN002)

| Accounts | Length | Assigned by | Examples (provisional, DN006) |
|----------|--------|-------------|-------------------------------|
| System (`< 2²⁰`) | 1–3 letters | The genesis block | `PPL` (genesis: the native token), `RSV`, `REG`, `FEE`, `RCY` |
| Issued (`2²⁰ … 2³⁰`) | 4–7 letters | The registrar | `ACME`, `SHOPCO` |
| User (`≥ 2³⁰`) | none | — | (later: `alice@ACME`, NAME_DIRECTORY's locals) |

- **Letters `A`–`Z` only**, stored upper-case; case-insensitive on input; shown lower-case in domain
  form (`acme`). No digits or punctuation (lookalikes: `0`/`O`, `1`/`I`).
- The length tells the kind: one to three letters is always the protocol's.
- An issuer's token is the one with its own id, so its domain is its token's symbol; the genesis
  account issues the native token, so its domain is the native token's symbol.

## The rules

### Read-only (DN003)

A domain, once given, never changes: renewals and the account's own updates carry it unchanged; no
rename, no transfer, no release. An account has at most one.

- **System accounts:** named in the genesis block (the genesis record and the system accounts'
  records carry them), never later.
- **Issued accounts:** the registrar names them — when it creates the account (`T_NEW_USER` carries
  the domain), or later for one created without (`T_DOMAIN_ASSIGN`, record type 7, signed by the
  registrar; its fee from the registrar).
- **User accounts** have none.

### Retired, never reused (DN004)

When an issuer with a domain ends (it could not pay its renewal), its domain is **retired**: bound
to its old account for good, never given again — a dead company's ticker never comes back on someone
else's token. The registrar's account keeps the retired domains, in place (as miner state), so every
renewal of it carries them and a node joining from a checkpoint learns them.

### Proven both ways (DN005)

A wallet must check "`ACME` is account 1,048,626" without believing a node — a symbol a node can lie
about is worse than a number. Both directions are under the block's state root:

- **Account → domain:** the domain is in the account's wallet (`Wallet.domain`), so in its leaf.
- **Domain → account:** each domain has a leaf of its own in the same sparse Merkle tree, at a key
  no account can have: `2⁶³ + code(domain)` (letters as base-27 digits, `A` = 1 … `Z` = 26). The leaf
  commits `(domain, account, retired)`. Account ids are below 2⁶³ from now on.

`DOMAIN_GET` (request 2101, reserved for it) answers with the leaf and its Merkle path, like
`ACCOUNT_PROOF`; `verify::checkDomainProof` checks it against a verified block. An absent domain is
proven absent.

### Where a domain resolves (later)

These domains are this chain's namespace. They are not internet top-level domains — three letters
collide with `COM`, `ORG`, `UK`, and four to seven with newer ones — so DNS service will serve them
under a suffix this project owns, or natively in pp-browser; the choice is open (no DNS in this
project).

## Wire

- `Wallet.domain` (string; empty: none); the registrar's `Wallet.retiredDomains` (domain, account).
  Account records change form: a new genesis (no network is live).
- Record type 7 `T_DOMAIN_ASSIGN` (`TxDomainAssign`: `walletId`, `domain`, `fee`, idempotency and
  validity window), signed by the registrar.
- `T_REQ_DOMAIN_GET` (2101): payload the domain; reply `DomainProof`.
- `pp-client`: `domain <NAME>` and `verified-domain <NAME> --genesis-hash`; `op new-account
  --domain`; `op assign-domain --account A --domain NAME`; genesis init config: the system names.

## In pp-ledger-apps

- **Registrar:** a domain for each issued account it creates; assigning one to an existing account.
- **Wallet, token app:** symbols shown with amounts (`1,000 ACME`), proven; send by symbol; an account
  found by its domain.
- **Genesis app:** the system names in the genesis plan.

## Phases

[PHASES.md](PHASES.md): the state and rules (dn1), proofs and pp-client (dn2), the apps (dn3),
docs (dn4).
