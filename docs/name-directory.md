# Name directory — domains, locals, and account attachments

**Status:** Accepted design (schema / wire only; no runtime yet)  
**Date:** 2026-09-03  
**Related:** [design.md](design.md) (accounts & reserved band), [platform-integration.md](platform-integration.md) (pp-node `ledger_gateway`, mesh edge), [ledger-topology.md](ledger-topology.md) (terminal-owned indexes)

Cross-repo phone-book north star (pp-browser): N029 / `NAME_DIRECTORY_NORTH_STAR.md` — HTTP/Amp directory now; **this chain registry is eventual name authority**.

This document freezes how **pp-ledger** supports memorable names and optional account-attached posts. It does **not** schedule implementation.

---

## 1. Goal

Users find each other by a memorable handle under a **registerable domain**. pp-ledger’s job is **name ownership** and **account-attached blob storage** — not mesh dialing.

```text
Bob → NAME_GET(alice@domain)     // NameIndex lookup (not a chain scan)
   → Alice’s ledger wallet id
   → ACCOUNT_GET / attachment    // opaque blob: PeerIds, Brief account id, posts, …
   → (pp-browser interprets blob and dials PeerId)
```

| Concern | Who cares |
|---------|-----------|
| Who may hold `alice@domain` | **pp-ledger** (DomainIndex / NameIndex / grants) |
| Brief Account ID, PeerId, endpoints, post hashes | **Account attachment** — stored/returned by ledger; **not** ledger protocol keywords |
| Amp dial / DHT / contacts | **pp-browser** (and projectors), using PeerId from the blob |

You cannot Amp-dial a Brief Account ID or a ledger wallet id. Mesh reachability is always **PeerId**; the chain path above is how Bob discovers those PeerIds after a name lookup.

---

## 2. Identity layers (do not collapse)

| Layer | Form | Role | First-class on chain? |
|-------|------|------|------------------------|
| **Memorable name** | `local` + `domain` → `local@domain` | Human entry; unique under DomainGrant | **Yes** — NameIndex |
| **Ledger account** | `uint64` wallet id | Owner of the name; spend / renew / close | **Yes** — AccountBuffer |
| **Account attachment** | versioned blob on the wallet | Rich tips consumers may publish | **Opaque storage** (once persisted) |
| **PeerId / endpoints** | inside attachment | What mesh actually dials | No — consumer-defined |
| **Brief Account ID** | inside attachment | Optional person link for pp-browser | No — consumer-defined |

**Chain primary for names:** `local@domain` → `uint64`.  
**Mesh dial target (consumer):** PeerId(s) read from attachment (or HTTP/Amp twin until chain tips exist).  
**Memorable name** is not a substitute for wallet id; **Brief Account ID** is not a mesh address.

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
  seq
  expires_at?               // optional; may follow account upkeep
```

Brief Account ID / PeerId / endpoints are **not** NameBinding fields — they live in the owner’s account attachment (opaque to ledger semantics).

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

## 6. Account attachment — multi-profile envelope (hybrid)

From pp-ledger’s point of view, attachment **payload bytes are opaque** (no PeerId / Brief / post semantics). The chain **does** mandate a thin envelope, **wallet-id profile keys**, size caps, and optional **attested** slots. The account owner has **full authority** on-chain to write any slot on their own attachment.

Once `NAME_GET` yields `owner_wallet_id`, clients `ACCOUNT_GET` and read `attachment.profiles[…]`.

### 6.1 Envelope (mandated)

```text
AccountAttachment
  version: 1
  profiles: {
    <publisher_wallet_id>: ProfileSlot,   // uint64 — must exist on chain
    ...
  }

ProfileSlot
  mode: "user" | "attested"   // or infer from attestation presence
  data: bytes | object        // opaque to ledger meaning
  seq: u64                    // per-slot
  attestation?: {             // required when mode = attested
    publisher_wallet_id       // must equal the map key; reserved (< ID_FIRST_USER)
    domain                    // required for attested; DomainIndex owner == publisher
    expires
    sig                       // publisher signs (profile_id, data, seq, subject_wallet, …)
  }
```

**`profile_id` = publisher’s ledger wallet id** (not a free-form string). Avoids apps squatting “good” names; display uses `NAME_GET_BY_WALLET(publisher)` / domain when useful. Multiple products from one publisher live **inside** that slot’s `data` (publisher-controlled), not as extra map keys.

**Ledger always validates:** `version`, map shape, each key is an existing wallet id, total/per-slot byte caps, fee.  
**Ledger validates when attested:** map key == attestation.publisher; publisher reserved; domain owned by that publisher; signature; expiry.  
**Ledger never validates:** contents of `data`; and for `mode=user`, does **not** require the publisher’s signature — the **subject account owner** alone may write any key on their profile.

Illustrative policy knobs: `max_attachment_bytes`, `max_slots`, `max_slot_bytes`.

### 6.2 Slot kinds and authority

| Kind | On-chain write authority | Extra chain check | Reader trust |
|------|--------------------------|-------------------|--------------|
| **user** | Subject (Alice) signs outer tx; may set any `profiles[P]` | Envelope + caps + key exists | Do **not** treat as “officially from P” |
| **attested** | Alice still publishes | + reserved publisher + domain attestation | Safe to treat as vouched by that org |

Alice owns her attachment: chain sovereignty is **hers**. Attribution for official tips is **`attested` only**. Indie apps use a normal user wallet as `profile_id` without reserved/domain; they are not attested.

```text
profiles[<reserved_pp_wallet>] = {
  mode: "attested",
  data: { mesh_tip, external_account_id?, posts_tip? },  // e.g. pp-browser consumer schema
  seq: 7,
  attestation: { publisher: <reserved_pp_wallet>, domain: "example.com", sig: … }
}

profiles[<indie_app_wallet>] = {
  mode: "user",
  data: { … },
  seq: 3
}
```

### 6.3 Wallet host — keys stay with the user; proposal auth is host-side

Apps never receive account private keys. **Publisher proposal authentication is host policy, not a chain write rule:**

```text
1. App P → host: proposal { profile_id: P, data [, attestation] }
   Host SHOULD verify the proposal is signed by P (anti-phishing / “really from this app”)
2. Host loads current attachment; merges **that key only**
3. Review UI: slot diff, fee/size, mode (user vs attested), “other profiles untouched”
4. User confirms (PIN) → host signs ATTACH_UPSERT_SLOT → TX_ADD
5. App gets ack — still no keys
```

Alice may also edit any slot via a power editor / `ATTACH_REPLACE_ALL` (warned). Chain allows it; host UX should make wipe risk obvious.

Default app path: replace `profiles[P]` only. Full-map replace is an explicit escape hatch.

### 6.4 Validation split (chain vs client)

| Concern | On-chain | Wallet / host | Consumer when reading |
|---------|----------|---------------|------------------------|
| Name / domain ownership | Yes | — | — |
| Envelope, caps, slot-patch, key∈wallets | Yes | Merge preview | — |
| Alice may write any `profiles[P]` | Yes (her sig) | Optional proposal-from-P check | — |
| Attested reserved+domain sig | Yes when mode=attested | Show publisher/domain | Prefer attested for official tips |
| PeerId / Brief / post meaning | No | Interpret known publishers’ `data` | Same |
| Phishing | No | Slot-scoped review + proposal auth | Ignore non-attested for high trust |

Rule of thumb: **Alice’s blob is hers on-chain**; **“did this proposal come from app P?”** is host-side; **“is this an official org tip?”** is attested + consumer policy.

### 6.5 Consumer data example (under a known publisher wallet)

Documented for pp-browser — **not** ledger consensus keywords. Slot key = platform/reserved publisher wallet id; `data` shape e.g.:

```text
data:
  external_account_id?   // Brief "account:…"
  mesh_tip?              // peer_id / endpoints[] / entity_kind / capabilities
  posts_tip?             // content hashes + parents, seq — no global PostIndex
```

Other apps use **their** wallet id as `profile_id` and their own `data` schema.

### 6.6 Live state (hard cut; buffer stays light)

`AccountBuffer::Account` remains `{ id, wallet, blockId }` only — no attachment bytes in the hot map (scale).

On write (`T_NEW_USER` / `T_USER_UPDATE` / genesis+config): validate `UserAccount.meta` as an `AccountAttachment` envelope and store it **in the tx / tip block** only.

On read (`ACCOUNT_GET`, renewal fee/meta): hydrate attachment **ad-hoc** from the account tip (`blockId` → last user/genesis meta record). Optional disk side-index later; not in the consensus buffer.

Hard cut (no dual decode): free-text meta is rejected; empty input canonicalizes to an empty v1 envelope. Wipe old ledgers before upgrading.

**Reserved ids (not wired yet):** `Ledger::T_DOMAIN_*` / `T_NAME_*` / `T_ATTACH_*` (7–15); `Client::T_REQ_DOMAIN_GET` / `NAME_GET` / `NAME_GET_BY_WALLET` (2101–2103).

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
| `ACCOUNT_GET` | existing; later returns persisted `AccountAttachment` |
| `TX_ADD` | domain/name txs; attachment slot upsert/delete |

Exact numeric `T_REQ_*` ids assigned at implementation time.

### Tx family (logical — types TBD)

| Tx | Signer | Effect |
|----|--------|--------|
| `DOMAIN_CLAIM` / renew / release | Reserved owner | Mutate `DomainIndex` |
| `NAME_CLAIM` / renew | Beneficiary (+ embedded `DomainGrant`) | Mutate `NameIndex` |
| `NAME_RELEASE` | Domain owner and/or beneficiary (policy) | Free the local |
| `ATTACH_UPSERT_SLOT` | Account owner | Patch one `profiles[id]` (other keys unchanged) |
| `ATTACH_DELETE_SLOT` | Account owner | Remove one profile key |
| `ATTACH_REPLACE_ALL` | Account owner | Replace entire map (warned escape hatch) |

Prefer **slot upsert** as the default path so one app-driven update does not replace the whole map. Prefer **dedicated name txs** over stuffing uniqueness into free-form meta; indexes remain authoritative for collisions.

For `ATTACH_UPSERT_SLOT`: account owner signature is sufficient for `mode=user` (any existing publisher key). For `mode=attested`, chain additionally verifies reserved + domain attestation (map key must be that publisher).

---

## 8. Mesh / pp-browser mapping

pp-browser (or any consumer) uses ledger as a phone-book backend behind `INameDirectory`:

```text
NAME_GET(alice@domain)  →  NameBinding { owner_wallet_id, … }
ACCOUNT_GET(wallet)     →  AccountAttachment.profiles
Consumer picks a publisher wallet (e.g. platform reserved id), reads
  profiles[<publisher_wallet>].data  (prefer mode=attested for dial-critical tips)
  → mesh_tip / external_account_id / …
→ dial PeerId (DHT / contacts for multiaddrs)
```

| NameRecord field | Chain source |
|------------------|--------------|
| `name` | canonical `local@domain` (NameIndex) |
| `peer_id` / `endpoints[]` | attested (or chosen) publisher slot `data` / HTTP/Amp projector |
| `account_id` | same consumer `data` (opaque) |
| `entity_kind` / `capabilities` | same / projector |
| `seq` / `expires_at` | NameBinding (+ slot `seq` as needed) |

Do not treat a non-attested `profiles[P]` as proof that P authored the tip — only Alice committed it to her blob.

HTTP/Amp directories become projectors/caches once chain tips exist. DHT must not store names.

Public edge remains **pp-node `ledger_gateway`**; terminal `pp-beacon` stays scarce / often private ([platform-integration.md](platform-integration.md)).

---

## 9. Anti-blockers

1. Do not make `local@domain` the ledger account id.  
2. Do not let users register domains.  
3. Do not enforce domain/name uniqueness only via free-form account meta — use DomainIndex / NameIndex.  
4. Do not treat Brief Account ID or PeerId as ledger protocol keywords; keep them in slot `data`.  
5. Do not imply Amp can dial wallet id or Brief Account ID — dial target is PeerId from consumer data.  
6. Do not require multiaddrs on NameBinding (hints belong in attachment / DHT).  
7. Do not put a global PostIndex in the critical path for v1.  
8. Do not ship name transfer in v1.  
9. Do not treat vanity encodings of `uint64` as the memorable namespace.  
10. Do not embed libp2p or mesh dial logic inside pp-ledger core.  
11. Do not document name lookup as a full-chain scan — it is an index RPC (`NAME_GET`).  
12. Do not use free-form string profile ids — keys are existing wallet ids.  
13. Do not require every app slot to be attested — attested is reserved+domain only.  
14. Do not require publisher signature on-chain for `mode=user` — subject owner has full write authority on their attachment.  
15. Do not treat non-attested `profiles[P]` as proof P authored the tip.  
16. Do not hand account private keys to apps — host proposes/reviews; user signs.  
17. Do not default to full attachment replace for multi-app updates — prefer `ATTACH_UPSERT_SLOT`.  
18. Do not expect chain validation to stop phishing — host proposal auth + review.

---

## 10. Implementation posture

**Now:** envelope validate on write + ad-hoc hydrate on read; reserved tx/RPC ids.  
**Later:** DomainIndex / NameIndex + grant validation + name/attach RPC; attested crypto + DomainIndex check; slot upsert txs; optional attachment tip index for gateways; browser chain provider.  
**Not blocking:** PostIndex, name transfer, requiring attestation for all apps, vanity encoding.

---

## Decision log

| Date | Decision |
|------|----------|
| 2026-09-03 | Memorable names are `{local, domain}` with canonical `local@domain` |
| 2026-09-03 | NameIndex maps name → ledger `uint64`; Brief Account ID / PeerId live in opaque slot `data` |
| 2026-09-03 | Mesh dial target is PeerId (consumer); chain does not dial and does not treat PeerId as a keyword |
| 2026-09-03 | Only reserved accounts register domains; user locals require DomainGrant |
| 2026-09-03 | No first-class PostIndex v1 — posts tip in consumer profile data |
| 2026-09-03 | Attachment = multi-profile envelope; chain validates structure/caps; payload opaque |
| 2026-09-03 | `profile_id` = existing publisher wallet id (no free-form string squat) |
| 2026-09-03 | Subject account owner has full on-chain authority over their attachment slots |
| 2026-09-03 | Publisher proposal auth is host-side; attested (reserved+domain) is the on-chain authenticity bit |
| 2026-09-03 | Default publish path = slot upsert via wallet host; user signs; apps never hold keys |
| 2026-09-03 | Hard cut: AccountAttachment envelope on writes; hydrate ad-hoc from tip block (buffer stays light); reserve T_* 7–15 and T_REQ 2101–2103 |
