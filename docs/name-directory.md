# Name directory — domains, locals, and account attachments

**Status:** Accepted design (schema / wire only; no runtime yet)  
**Date:** 2026-09-03  
**Related:** [design.md](design.md) (accounts & reserved band), [platform-integration.md](platform-integration.md) (pp-node `ledger_gateway`, mesh edge), [ledger-topology.md](ledger-topology.md) (terminal-owned indexes)

Cross-repo phone-book north star (pp-browser): N029 / `NAME_DIRECTORY_NORTH_STAR.md` — HTTP/Amp directory now; **this chain registry is eventual name authority**.

This document freezes how **pp-ledger** supports memorable names and optional account-attached posts. It does **not** schedule implementation.

---

## 1. Goal

Users and services should be findable by a memorable handle under a **registerable domain**, without teaching PeerIds or raw wallet ids as the primary UX.

```text
alice@example.com  →  ledger wallet (uint64)  →  optional mesh tips / posts in account attachment
```

Mesh consumers (pp-browser) map that wallet (and optional Brief `account:` link) to PeerId/endpoints via the existing name-directory port. The chain owns **who may hold which name**; reachability stays off the authoritative name record.

---

## 2. Identity layers (do not collapse)

| Layer | Form | Role | Stable? |
|-------|------|------|---------|
| **Ledger account** | `uint64` wallet id | Spend, renew, close, sign grants | Yes (keys rotate without changing id) |
| **Brief Account ID** | `account:…` (optional link field) | Mesh / contacts person root | Yes until account key rotation |
| **Memorable name** | `local` + `domain` → canonical `local@domain` | Human entry into the phone book | No — rebind / release allowed |
| **PeerId / multiaddrs** | device / dial hints | Reachability only | Device-scoped; not name truth |

**Primary economic key on chain:** `uint64`.  
**Primary person key for mesh:** Brief Account ID when linked.  
**Memorable name:** claimable label under a domain, not a substitute for either id.

Optional **vanity digit↔letter encoding** of the `uint64` (phone-keypad style) is display/input sugar only — not a namespace and not a substitute for `local@domain`.

---

## 3. String form vs storage

| Layer | Rule |
|-------|------|
| **Storage / uniqueness key** | Split fields: `local`, `domain` (both normalized) |
| **Canonical string** | `local + "@" + domain` (e.g. `alice@example.com`) |
| **UI** | May show `alice@…` or `alice.…`; must parse to `{local, domain}` before RPC |

Do **not** store a free-form “any format” string as the authoritative key. Clients may accept alternate input and normalize.

**Normalization (v1 sketch — freeze before ship):**

- `domain`: lower-case ASCII / punycode; no `@`; product may fix one ship domain at genesis
- `local`: lower-case; allowed charset TBD (alphanumeric + limited punctuation); no `@`
- Empty `local` reserved for domain-owner default address if needed later

---

## 4. Domains — reserved accounts as issuers

pp-ledger already separates:

- **Reserved band:** `id < ID_FIRST_USER` (`1<<30`) — genesis, fee, reserve, recycle, token issuers  
- **User band:** `id >= ID_FIRST_USER`

**Domain ownership mirrors token issuance:** a **reserved account** may claim a **unique domain**. Normal user accounts cannot register domains.

```text
DomainIndex   domain → reserved_wallet_id     // unique; reserved-only writes
NameIndex     (local, domain) → user_wallet_id // unique; requires domain-owner grant
```

| Who | Privilege |
|-----|-----------|
| Genesis / reserved | Create reserved accounts; claim unused domains |
| Domain owner (reserved) | Grant / revoke locals under its domain; renew / release domain |
| User account | Hold `local@domain` only with a valid **DomainGrant** |
| Anyone (RPC) | `DOMAIN_GET` / `NAME_GET` |

Same reserved wallet **may** also be a token issuer; domain and token are separate indexes sharing the same privilege band.

Product ship domain (fixed TLD at launch) should be pre-registered at genesis or by a dedicated reserved account so the public namespace exists before org domains.

### DomainBinding (wire sketch)

```text
DomainBinding
  domain              // normalized
  owner_wallet_id     // must be reserved (< ID_FIRST_USER)
  seq
  expires_at          // optional lease; may track account renewal
```

---

## 5. Locals — owner-approved name claims

A user name binding is:

```text
NameBinding
  local
  domain
  // derived key: local + "@" + domain
  owner_wallet_id           // user id (>= ID_FIRST_USER)
  external_account_id?      // optional Brief "account:…"
  seq
  expires_at?               // optional; may follow account upkeep
```

### DomainGrant (approval)

Approval is **owner-signed**, not an on-chain allowlist scan:

```text
DomainGrant
  domain
  local
  beneficiary_wallet_id
  expires                   // slot or unix window
  nonce / seq
  signature                 // domain owner wallet keys (ML-DSA-65)
```

`NAME_CLAIM` (or equivalent tx) carries the grant. Chain checks:

1. `DomainIndex[domain] ==` grant signer’s wallet  
2. Signer is that reserved account and domain lease is live  
3. `(local, domain)` free (or same beneficiary renewing)  
4. `beneficiary_wallet_id >= ID_FIRST_USER`  
5. Grant not expired; nonce not reused  

This parallels `T_NEW_USER` sponsorship (`from` authorizes / pays; `to` is created): **domain owner authorizes the name**; funding the user account can be a separate party.

**Non-goals for v1:** name transfer between users; squatting auctions; user-owned domains.

**Revoke / rename:** domain owner releases or re-grants; user obtains a new grant for a new local.

---

## 6. Account attachment — posts without a PostIndex

Posts (content hashes, parent hashes) do **not** need a first-class global post index for v1.

Once `NAME_GET` yields `owner_wallet_id`, clients load the account and decode a **persisted attachment**:

```text
AccountAttachment (versioned)
  external_account_id?
  posts_tip?              // root hash and/or small hash list + parent refs, seq
  mesh_tip?               // optional entity_kind / capabilities / endpoint hints
  // other profile fields…
```

| Concern | Approach |
|---------|----------|
| Discover author | Name / wallet id |
| Bodies | Off-chain; chain holds hashes the owner currently asserts |
| History | Tip / short commitment set in attachment — not unbounded archive |
| Global `POST_GET` by hash | Deferred; promote to PostIndex only if needed |

### Gap vs today’s runtime

Live `AccountBuffer::Account` is `{ id, wallet, blockId }` only. `UserAccount.meta` rides on txs for fees/history but is **not** kept as live account state. Account attachment (and therefore posts-in-meta) requires a deliberate state change when implementing — do not assume decode-after-`ACCOUNT_GET` works today.

---

## 7. State & RPC surface

### Terminal-owned indexes

Same freshness model as other terminal registries ([ledger-topology.md](ledger-topology.md)): gateways replicate; mutations write through to the terminal.

```text
Terminal beacon state
  AccountBuffer
  DomainIndex
  NameIndex
  // PostIndex — not required for v1
```

### RPC sketch (uniform on beacon / relay)

| Request | Purpose |
|---------|---------|
| `DOMAIN_GET` | by `domain` → `DomainBinding` |
| `NAME_GET` | by canonical `local@domain` → `NameBinding` (+ optional wallet summary) |
| `NAME_GET_BY_WALLET` | reverse: wallet → preferred name(s) |
| `ACCOUNT_GET` | existing; later returns attachment when persisted |
| `TX_ADD` | claim/renew/release domain; claim/release name (with grant) |

Exact numeric `T_REQ_*` ids assigned at implementation time.

### Tx family (names only — types TBD)

| Tx (logical) | Signer | Effect |
|--------------|--------|--------|
| `DOMAIN_CLAIM` / renew / release | Reserved owner | Mutate `DomainIndex` |
| `NAME_CLAIM` / renew | Beneficiary (+ embedded `DomainGrant`) | Mutate `NameIndex` |
| `NAME_RELEASE` | Domain owner and/or beneficiary (policy) | Free the local |

Prefer **dedicated tx types** (or one typed family) over stuffing uniqueness rules into opaque `UserAccount.meta` alone. Meta/attachment may **mirror** display fields; indexes are authoritative for collisions.

---

## 8. Mesh / pp-browser mapping

```text
Cold start → ledger_gateway (pp-node) → NAME_GET(alice@domain)
         → NameBinding { owner_wallet_id, external_account_id? }
         → ACCOUNT_GET / directory twin → PeerId hints
         → dial (DHT / contacts for multiaddrs)
```

Chain-era `NameRecord` (browser port) should map 1:1:

| NameRecord field | Chain source |
|------------------|--------------|
| `name` | canonical `local@domain` |
| `account_id` | `external_account_id` or derived link |
| `peer_id` / `endpoints[]` | attachment `mesh_tip` or HTTP/Amp projector |
| `entity_kind` / `capabilities` | attachment or projector |
| `seq` / `expires_at` | `NameBinding` |

HTTP/Amp directories become projectors/caches once chain is truth. DHT must not store names.

Public edge remains **pp-node `ledger_gateway`**; terminal `pp-beacon` stays scarce / often private ([platform-integration.md](platform-integration.md)).

---

## 9. Anti-blockers

1. Do not make `local@domain` the ledger account id.  
2. Do not let users register domains.  
3. Do not enforce domain uniqueness only via free-form account meta.  
4. Do not require multiaddrs on the authoritative name record.  
5. Do not put a global PostIndex in the critical path for v1.  
6. Do not ship name transfer in v1.  
7. Do not treat vanity encodings of `uint64` as the memorable namespace.  
8. Do not embed libp2p or mesh dial logic inside pp-ledger core.

---

## 10. Implementation posture

**Now:** this design doc + keep browser `INameDirectory` / `NameRecord` aligned.  
**Later:** DomainIndex / NameIndex + grant validation + RPC; persist `AccountAttachment`; wire browser chain provider behind the same port.  
**Not blocking:** PostIndex, name transfer, multi-domain UI polish, vanity encoding.

---

## Decision log

| Date | Decision |
|------|----------|
| 2026-09-03 | Memorable names are `{local, domain}` with canonical `local@domain` |
| 2026-09-03 | Ledger `uint64` remains economic primary; Brief Account ID optional link; name is mutable label |
| 2026-09-03 | Only reserved accounts register domains; user locals require DomainGrant |
| 2026-09-03 | No first-class PostIndex v1 — posts tip in persisted account attachment |
| 2026-09-03 | Design/wire only; runtime deferred |
