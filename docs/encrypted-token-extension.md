# Encrypted Token Extension: Monero-Style Privacy on an Account-Based Ledger

## Executive summary

This note describes an **encrypted token** extension that aims for **Monero-like privacy** (hiding sender, receiver, and amount) on our chain, while adding **post-quantum (PQ) hardening** and staying compatible with **account-based** state and **checkpoint-based** expiry of old pending transactions. Verification leans on **zk-STARK**-style proofs (as used in ecosystems such as Starknet) so the network can validate rules without revealing sensitive details. **AI-managed wallets** are discussed as an **optional** way for **very high-value** users to operate **many accounts** efficiently and further obscure **aggregate** wealth beyond per-account privacy.



---

## 1. What we’re building

### Goal

A token type where, by default:

- **Who sent** is not publicly identifiable in the usual way  
- **Who received** is not publicly tied to a clear on-chain “to address”  
- **How much** moved is not visible on-chain

The network must still enforce validity: authorization, conservation of value, no double-spend, and compliance with our ledger rules.

### Why it matters

Strong privacy supports payments, payroll, merchant settlement, and treasury use cases where public balance and flow visibility is a liability. Pairing that with **PQ-oriented design** addresses long-term security expectations as quantum computing matures.

### High-value users: AI-managed wallets and many accounts

For **extremely high-value** individuals and institutions, a practical complement to cryptographic privacy is to **fragment wealth across many accounts**, so no single on-chain position reveals **aggregate** holdings. Manual operation does not scale to hundreds or thousands of accounts. **AI-managed wallets**—custody software that automates key hygiene, private receive discovery, checkpoint-aware scheduling, and routing across accounts—can make **large-scale multi-account privacy** workable in practice. The protocol does not mandate this; it is an **optional custody pattern** on top: the same privacy rules apply per account, while intelligent agents **orchestrate** many accounts efficiently for one beneficial owner.

---

## 2. Design pillars: privacy model, zk-STARK verification, PQ KEM

### Monero-style privacy (conceptual)

Monero combines mechanisms so observers cannot reliably link senders, recipients, and amounts. We target **similar user-facing properties**, adapted to our **account-based** model (see Section 4).

### zk-STARKs

We use **zk-STARK**-class proofs so validators can check:

- The transaction satisfies protocol rules  
- The sender is authorized  
- Balances remain consistent (no negative balances, no double-spend)

…**without** publishing sender, receiver, or amount in plaintext.

**Why zk-STARKs are attractive here**

- **No trusted setup** (often important for adoption and trust)  
- **PQ-friendly proof assumptions:** STARK-style systems are built primarily on **hash-based** and algebraic ingredients that are **not vulnerable to Shor-style attacks** in the way **elliptic-curve discrete-log** SNARKs are. That **aligns the zero-knowledge layer with a post-quantum threat model**—complementary to the PQ KEM used for encryption and key agreement (see below).  
- Strong tooling and ecosystem momentum  
- A path toward scalable verification (aggregation, optimized verifiers)

### PQ KEM (post-quantum key encapsulation)

Classical curves may be vulnerable to future quantum attacks. A **PQ KEM** layer strengthens privacy-related encryption and key agreement against that class of threat.

**Positioning:** privacy that is intentionally aligned with a **post-quantum** threat model, not only classical adversaries.

---

## 3. Account-based ledger, checkpoints, and expiry

We remain **account-based** (balances attached to accounts), which keeps:

- Familiar account semantics  
- Alignment with existing ledger and integration patterns  
- Easier application and exchange integration than a pure UTXO redesign  

### Why account-based (including key lifecycle)

Authorization is anchored on **accounts**, not on a growing graph of one-off coins. That shape helps **key rotation and upgrades**:

- Users can **move control** to new keys (e.g. after a compromise, a policy change, or a **post-quantum migration**) through a defined account-level update, instead of having to **sweep** many historical outputs or trace funds through a long UTXO history.  
- Wallets and custody flows can standardize on **“this account is now authorized by this key material”**, which is simpler to reason about than per-output migration for every past receive.

The privacy layer still has to hide flows and amounts; the point is that **the ledger’s notion of “who may spend” stays account-centric**, which keeps operational key management tractable as cryptography evolves.

### Checkpoints and hanging transactions

We use **checkpoints**. After a checkpoint is finalized, **older transactions that never settled** (sometimes called “hanging” or stale pending work) **expire**.

**Why this helps**

- Caps unbounded growth of pending privacy-related state  
- Makes resource use more predictable for nodes (memory, validation load)  
- Reduces abuse surface from indefinite backlog or replay-style games  

### Why checkpoint-based (chain data and cost)

Boundaries in time let the protocol **retain only what validators must still enforce**. Privacy schemes often need extra structures (e.g. anti-replay or nullifier sets, pending buffers). Without checkpoints, that auxiliary state can grow with the full history of the chain.

Checkpoints **limit the horizon** of what must be kept “live” for correctness. That tends to:

- **Shrink the working set** nodes need for validation and sync—not the entire past of every dangling intent forever  
- **Lower storage and bandwidth** for operators who need to catch up or run lean infrastructure  
- **Improve steady-state performance** and **reduce operating cost** compared to an unbounded historical obligation for every privacy artifact

Together, **account-based state** (compact current authorization and balances) and **checkpoint-scoped retention** (expire stale work) push toward a **lighter long-term chain footprint** than designs where every privacy detail must remain indefinitely replayable.

---

## 4. Hiding sender, amount, and receiver under account-based semantics

We still aim to hide **sender**, **amount**, and **receiver** from public observation, but **receiver handling differs** from classic UTXO systems: there is no natural “new output coin” in the same form; credits land as **account balance updates**.

### High-level behavior (non-technical)

- The sender builds a transaction that includes **encrypted recipient-related data** and a **zero-knowledge proof** that the credit is valid under protocol rules.  
- The receiver uses private material to **discover and accept** incoming transfers without the chain publicly labeling them as the recipient in the traditional sense.  
- The chain relies on **proof verification** and protocol rules rather than visible amounts and addresses.

This is the main **engineering and cryptography** hotspot: getting receiver discovery, accounting, and anti-abuse right in an account-based setting.

---

## 5. Trade-offs and drawbacks

Privacy and PQ-oriented design have real costs. Expect:


| Area                 | Typical impact                                                                                         |
| -------------------- | ------------------------------------------------------------------------------------------------------ |
| **Fees / compute**   | Higher than plain transfers: proving and verifying adds work                                           |
| **Transaction size** | Larger payloads (ciphertext, proofs, auxiliary data)                                                   |
| **Node memory**      | Extra structures (e.g. key-image-like anti-double-spend state) retained or managed by miners/verifiers |
| **Complexity**       | More moving parts: circuits, PQ integration, wallet UX, audits                                         |


This is the usual privacy trade: **strong confidentiality vs. cost and operational complexity**.

---

## 6. Supplemental product: “semi-private” token

**Full** privacy is not the right fit for every user or organization. Some stakeholders—treasury teams, boards, regulators, partners, or individuals—want **private transfers** but remain uneasy about **permanent, total opacity** of balances and positions. For them we define a **supplemental** token class that sits **alongside** the primary encrypted token rather than replacing it or deferring delivery.

### Definition

- **Transfers remain private** in the sense that ordinary chain observers do not see classic sender/receiver/amount transparency,  
- but **account balances are disclosed on a fixed schedule** (e.g. per epoch or rolling window).

### Why offer it

- Gives a **deliberate choice** between maximum confidentiality and **bounded transparency** of balances, without abandoning transfer privacy.  
- Aligns with **governance, auditing, and compliance** workflows that assume periodic visibility of aggregate or per-account positions.  
- Reduces operational and bookkeeping complexity for teams that need **explainable** privacy—strong flow privacy with **scheduled** balance clarity.

This is **product breadth**, not a backup plan: the semi-private token addresses a **segment** that would otherwise avoid the ecosystem entirely, while the fully private token remains the flagship for those who want it.

---

## 7. Engineering scope: difficulty and focus areas

### Rough difficulty


| Dimension                            | Assessment                                   |
| ------------------------------------ | -------------------------------------------- |
| R&D and protocol design              | High                                         |
| Implementation                       | High                                         |
| Operations (memory, validation load) | Medium–high                                  |
| External review                      | Essential before production-grade deployment |


### Focus areas

1. **Cryptography and protocol**
  - Soundness: unlinkability, confidentiality, double-spend prevention in an account-based privacy model  
  - Clear boundaries: what PQ KEM protects vs. what relies on other assumptions
2. **Zero-knowledge stack**
  - Prover performance (wallets, batching)  
  - Verifier performance (nodes)  
  - Proof size and possible aggregation
3. **Ledger rules and checkpoints**
  - Safe expiry semantics (no accidental loss of funds; clear wallet behavior)  
  - Finality and reorg assumptions around checkpoints
4. **State and memory**
  - Key-image-like sets, pruning, and DoS resistance  
  - Growth limits and worst-case validation paths
5. **Wallets and UX**
  - Private receive discovery that is practical on real devices  
  - Recovery, backups, and checkpoint/expiry messaging  
  - **AI-assisted custody** (optional product pattern): safe orchestration of **many accounts** for high-value users who use fragmentation to **obscure total wealth**, without sacrificing policy or audit hooks where required
6. **Assurance**
  - Specification-driven tests, fuzzing of parsing and verification, and **external cryptographic audit** before mainnet-grade rollout

---

## 8. Key takeaways

- **Differentiation:** Monero-like privacy goals plus **zk-STARK** verification and **PQ-oriented** key establishment is a distinctive combination.  
- **Fit with the stack:** Staying **account-based** preserves platform direction and integration patterns.  
- **Honest costs:** Expect **higher fees**, **larger transactions**, and **more node memory** than transparent transfers—typical for strong privacy.  
- **Product breadth:** A **semi-private** supplemental token serves users and organizations that want private transfers but prefer **scheduled balance disclosure**—governance, oversight, or compliance—alongside the fully private offering.  
- **Custody at scale:** **AI-managed wallets** can operationalize **many-account** strategies for high-value holders—fragmenting balances so **aggregate wealth** is harder to infer—on top of per-account cryptographic privacy.  
- **Execution:** Success hinges on proof performance, receiver-side design in an account model, state growth, and rigorous security review.

