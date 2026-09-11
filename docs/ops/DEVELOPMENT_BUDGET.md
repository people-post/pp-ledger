# Development and budget plan

This document outlines deliverables, prioritization (must-have vs nice-to-have), phased roadmap, and indicative resource needs for **pp-ledger**. **Engineering is assumed entirely in-house** (no outsourced development or engineering contractors). It assumes **best available AI assistance** during development for implementation velocity, tests, and documentation—not as a substitute for **independent external audits** of consensus, cryptography, and wallet surfaces (audits are third-party reviews, not product engineering headcount).

---

## Clarified constraints (inputs to this plan)

### Network defense priorities

1. **Highest priority — correctness under adversarial inputs:** Prevent **fraudulent data injection** and invalid or malicious messages from affecting chain state, consensus safety, or honest nodes’ behavior. This includes strict validation, bounds, replay/double-spend handling, and consistent application of rules at every ingress point (peers, APIs, relays).
2. **Second priority — survive DDoS:** The network should remain **available enough to recover** and should not collapse permanently under volume attacks. **Service degradation under load is acceptable** (e.g. slower responses, shedding load, prioritization), provided correctness is preserved and the system can return to normal operation when the attack abates.

Implication for budgeting: invest first in **validation pipelines, fuzzing, adversarial test suites, and invariant checks**; then in **rate limiting, connection limits, backpressure, and operational playbooks** for high-volume abuse. Less emphasis on “always full performance under attack” than on **no wrong answers** and **eventual survival**.

### Encrypted token extension

- Treated as **engineering execution**, not open-ended research: target **zk-STARK verifier and prover quality** **comparable to** (or better than) what is achieved in **mature open-source** ecosystems that ship STARK-class systems—while implementing **brand-new codebase** in this project.
- **Ideas and algorithms** from public literature and OSS may inform the design; **code is not copied** from third-party implementations. Licensing, attribution, and clean-room boundaries should be maintained as a matter of policy.

---

## Deliverables overview


| Area                   | Summary                                                                                                                                                             |
| ---------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Base chain**         | Correct execution of existing transaction types; defense-in-depth against malicious inputs; then DDoS survival with graceful degradation.                           |
| **Encrypted token**    | Full and semi-privacy modes per product definition; new STARK-style stack meeting OSS-comparable quality bar; integrated with account-based ledger and checkpoints. |
| **AI-managed wallets** | Optional custody/orchestration layer for customers (multi-account, policy limits, auditability).                                                                    |
| **Entry points**       | Smartphone wallet apps and/or browser plugins as alternative surfaces to CLI.                                                                                       |


---

## Must-have vs nice-to-have

### 1. Base chain + network defense


| Tier                   | Scope                                                                                                                                                                                                                                                                                                                                                                            |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Must-have**          | Deterministic, spec-aligned validation for all supported transactions; no state corruption from malformed or adversarial messages; bounded resource use per message/connection; safe sync, mempool, and reorg behavior; CI and regression tests on critical paths; operational logging and metrics. **Primary defense budget** aligns with **correctness and fraud prevention.** |
| **Must-have (second)** | Under DDoS: **survival** via limits, shedding, and degradation paths; **no correctness shortcuts** when overloaded.                                                                                                                                                                                                                                                              |
| **Nice-to-have**       | Formal verification of selected subsystems; global-scale peer reputation; sustained peak throughput under simultaneous attack (beyond “survive and degrade gracefully”).                                                                                                                                                                                                         |


### 2. Encrypted token extension (engineering-led)


| Tier             | Scope                                                                                                                                                                                                                                                                                                                                                                                                    |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Must-have**    | Clear **privacy ladder** (e.g. semi-privacy milestone → stronger privacy milestone); complete transition rules; ZK verification within protocol limits; double-spend and balance rules enforced; client proving feasible for target usage; test vectors; migration and failure-mode documentation. **New implementation** with quality bar **≥ typical strong OSS STARK deployments** (no code copying). |
| **Nice-to-have** | Maximum privacy and PQ integration on day one; proof aggregation at launch; optional compliance/disclosure hooks for institutions.                                                                                                                                                                                                                                                                       |


### 3. AI-managed wallets


| Tier             | Scope                                                                                                                                                                      |
| ---------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Must-have**    | If shipped: bounded automation (policies, limits, user-visible intent); secure key handling per chosen trust model; audit logs; clear custody vs self-custody positioning. |
| **Nice-to-have** | Highly autonomous agents; natural-language control; deep cross-product personalization—each increases security and regulatory surface.                                     |


### 4. Smartphone apps and browser plugins


| Tier             | Scope                                                                                                                   |
| ---------------- | ----------------------------------------------------------------------------------------------------------------------- |
| **Must-have**    | **One** primary consumer path (mobile **or** extension) with secure storage story and core flows (send/receive/status). |
| **Nice-to-have** | Second platform; feature parity everywhere; app-store polish; deep hardware-wallet integration.                         |


---

## AI assistance (planning assumption)

AI tools are assumed to improve throughput on **implementation, tests, refactors, and internal docs** (order-of-magnitude **~1.2–1.6×** vs same team without such tooling). They do **not** replace:

- Independent audit of consensus and cryptography  
- Legal/compliance design for custodial or agent-like products  
- Protocol sign-off where correctness is existential risk

---

## Phased roadmap (suggested)


| Phase  | Focus                                                                                          | Outcome                                                                           |
| ------ | ---------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- |
| **A**  | Base chain: validation, invariants, adversarial testing; **correctness-first** network ingress | Production-grade **transparent** ledger behavior under malicious data             |
| **A′** | DDoS: limits, backpressure, degradation policies                                               | **Survival** under volume; acceptable slowdowns                                   |
| **B**  | Encrypted token: semi-privacy + testnet                                                        | Demonstrable privacy path with **new** STARK-class code at OSS-comparable quality |
| **C**  | Encrypted token: stronger privacy + external crypto audit                                      | Candidate for privacy-enabled release                                             |
| **D**  | One wallet surface (mobile or browser)                                                         | Retail-ready onboarding                                                           |
| **E**  | AI wallet MVP (optional)                                                                       | Differentiation for advanced users/treasuries                                     |


Phases **B** and **C** may overlap in staffing but should not merge **release discipline** (do not ship unaudited ZK to production targets).

---

## Team plan

This section translates the roadmap into **who** does the work, **when** further hires join, and **how effort shifts** across phases. Numbers are **planning FTE** (full-time equivalent); part-time roles are shown as fractions.

### Starting team (planning assumption)


| Assumption                                   | Detail                                                                                                                                                                                                                                                       |
| -------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Day-one headcount**                        | **3 senior full-time employees**, all **in-house** engineering (no contract dev shops or body-shop parallel teams).                                                                                                                                          |
| **Typical split** (example—not prescriptive) | **(1)** tech lead + beacon/miner/relay/ledger; **(2)** second senior on networking, ingress validation, and performance; **(3)** senior focused on cryptography / ZK direction or security-heavy review and fuzz strategy—exact titles flex with candidates. |
| **What “max budget” funds**                  | Additional **permanent hires** on accelerated timelines, **tools and infra**, **external audits**, and **legal/compliance** retainers as needed—not outsourced core coding.                                                                                  |


### Core roles


| Role                                           | Primary ownership                                                                                | Typical headcount | Notes                                                                                                    |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------ | ----------------- | -------------------------------------------------------------------------------------------------------- |
| **Tech lead / architect**                      | Cross-cutting design, consensus/ledger boundaries, release criteria, technical debt tradeoffs    | **0.5–1.0**       | Often merged with senior engineer in small teams; should exist as explicit capacity before scaling hires |
| **Senior systems engineer (C++ / node)**       | Beacon, miner, relay, networking, performance, correctness-first validation at ingress           | **1.5–2.5**       | Core of Phases **A** / **A′**; stays central through **B**–**C** for integration                         |
| **Cryptography / ZK engineer**                 | STARK-style prover/verifier stack, protocol integration, proof bounds, test vectors              | **1.0–1.5**       | Ramps up for **B**; peaks through **C**; can be two mid-level hires under a lead if budget allows        |
| **Security engineer (internal)**               | Threat modeling, fuzzing strategy, review of validation paths, coordination with external audits | **0.5–1.0**       | Part-time early; **≥0.5 FTE** sustained once ZK and wallets are in scope                                 |
| **Client engineer (mobile *or* browser)**      | Wallet UX, secure storage integration, API to node/relay, release pipelines for one platform     | **1.0**           | Starts Phase **D**; second platform adds **+0.5–1.0 FTE**                                                |
| **AI / backend product engineer**              | Policy engine, signing workflows, observability for AI wallet MVP                                | **0.5–1.0**       | Phase **E**; optional until wallet surface is stable                                                     |
| **Technical PM / program**                     | Milestones, dependency management, audit windows, stakeholder comms                              | **0.25–0.75**     | Scales with team size and investor reporting cadence                                                     |
| **DevOps / SRE (fractional)**                  | CI, release artifacts, test environments, load/degradation experiments                           | **0.25–0.5**      | Heavier during **A′** and wallet **D**                                                                   |
| **Legal / compliance (external or part-time)** | Custody wording, regional restrictions, vendor contracts                                         | **0–0.25**        | Rises with **consumer wallets** and **AI**-mediated signing                                              |


### Planning FTE by phase (steady-state target)

Approximate **sum of concurrent FTE** when each phase is in full swing—not cumulative person-months. Overlaps between phases (e.g. **B** starting before **A** closes) are normal. **Planning assumes three senior engineers at month zero**; totals below include fractional PM/DevOps where shown and **additional hires** as the roadmap progresses.


| Phase  | Focus                                      | Systems / C++ | ZK / crypto               | Security     | Client      | PM / DevOps              | **Total (range)** |
| ------ | ------------------------------------------ | ------------- | ------------------------- | ------------ | ----------- | ------------------------ | ----------------- |
| **A**  | Correctness, validation, adversarial tests | **2.0–2.5**   | **0–0.25**                | **0.25–0.5** | **0**       | **0.25–0.5**             | **~2.5–3.5**      |
| **A′** | DDoS survival, degradation                 | **1.5–2.0**   | **0–0.25**                | **0.25–0.5** | **0**       | **0.25–0.5**             | **~2.25–3.25**    |
| **B**  | Semi-privacy token + testnet               | **1.5–2.0**   | **1.0–1.5**               | **0.5**      | **0**       | **0.25–0.5**             | **~3.25–4.5**     |
| **C**  | Stronger privacy, audit prep               | **1.0–1.5**   | **1.0–1.5**               | **0.5–1.0**  | **0**       | **0.25–0.5**             | **~3.25–4.5**     |
| **D**  | One wallet surface                         | **0.75–1.25** | **0.5–1.0** (maintenance) | **0.25–0.5** | **1.0**     | **0.25–0.5**             | **~3.25–4.25**    |
| **E**  | AI wallet MVP (optional)                   | **0.5–1.0**   | **0.25–0.5**              | **0.25–0.5** | **0.5–1.0** | **0.75–1.25** (see note) | **~2.25–4.25**    |


*Tech lead* is included implicitly in Systems/C++ or ZK columns unless hired separately; if dedicated, add **~0.25–0.5 FTE** to each active phase. *Phase **E** — PM/DevOps column:* **0.25** PM, **0.25–0.5** DevOps/SRE, **0.25–0.5** AI/backend product engineer (signing policies, orchestration APIs).

### Hiring sequence (after the initial three seniors)


| Order | Hire / expand                                                                         | Rationale                                                                                                         |
| ----- | ------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| **1** | **Fourth** engineer: systems/C++ or dedicated **security** (by gap vs starting three) | Sustains parallel work on **A** / **A′** (validation, fuzzing, degradation) without burning out the founding trio |
| **2** | **ZK/cryptography** engineer (if not already covered at senior depth)                 | Ramp for **B** alongside ledger integration                                                                       |
| **3** | **Client** engineer (mobile **or** browser)                                           | When APIs and privacy testnet path are stable enough to commit to a wallet contract (**D**)                       |
| **4** | Additional **systems** or **ZK** (by bottleneck)                                      | **C** audit prep, performance, and remediation capacity                                                           |
| **5** | **AI wallet** engineer + **legal** touchpoints                                        | Only after custody/signing model for **E** is defined                                                             |


### Ways to run leaner (in-house)


| Approach                      | Tradeoff                                                                            |
| ----------------------------- | ----------------------------------------------------------------------------------- |
| **Defer Phase E** (AI wallet) | Reduces headcount and legal surface until **D** proves distribution                 |
| **One fewer client platform** | Matches must-have (“mobile **or** extension”); second platform stays nice-to-have   |
| **Fractional DevOps / PM**    | Common up to ~4–5 FTE engineering; beyond that, dedicated ops reduces calendar risk |
| **Narrower semi-privacy MVP** | Faster path to a testnet milestone; full privacy comes in a later hardening pass    |


---

## Minimum calendar time (in-house, three seniors at start)

This scenario targets **shortest practical wall-clock time** to must-have outcomes while keeping **all engineering in-house** and assuming **three senior employees** from **day one**. “Maximum budget” here means **accelerating additional hiring**, **premium tools and infrastructure**, **external audit scheduling and remediation budget**, and **legal retainers**—not outsourced coding teams.

**What money can compress:** time-to-hire for **4th+ FTE**, CI and test hardware, dedicated staging networks, **pre-booked** external audit slots, and buffer for **post-audit fix sprints**. The **initial trio** already provides **real parallelism** (e.g. validation, ZK direction, and review/fuzz strategy) without vendor handoffs.

**What money cannot fully remove:** **external audit queue and calendar** (often **2–4+ months** for a serious crypto/ZK review); **time for in-house integration** of ZK with ledger rules; **regulatory** lead time if Phase **E** or custodial patterns apply.

### Operating model at start (in-house)


| Element                | Typical approach                                                                                                                                                     |
| ---------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Core FTE (month 0)** | **3** senior engineers with explicit ownership split (see **Starting team**); all implementation stays in **this** codebase—**no engineering contractors**.          |
| **Parallelism**        | Workstreams split across the three (ingress/consensus, ZK/proof stack, security/tests/fuzzing)—same repo, shared review culture.                                     |
| **Growth**             | Use budget to hire **additional permanent engineers** as early as recruiting allows; optional **0.25–0.5** PM/program once headcount exceeds **3**.                  |
| **Audits**             | **External** crypto/consensus audits for Phase **C** (and as needed for consensus earlier)—book slots **early**; audits are **reviews**, not substitute engineering. |
| **Client (Phase D)**   | **In-house** client engineer (see hiring sequence); wallet UX and secure storage implemented by employees against your node APIs.                                    |


### Indicative calendar (compressed vs baseline)

Durations are **order-of-magnitude**; assume **no major** protocol rewrites. **Baseline** refers to the broader **Team plan** calendar (~**18–36 months** end-to-end for full must-have scope in typical conditions). **Compressed** assumes **strong** hiring velocity after the initial three, **pre-booked** audits, and disciplined scope (semi-privacy before full privacy where applicable).


| Phase            | Baseline-style calendar (single reference) | Compressed (in-house, budget for fast hiring + tools + audits) | Main levers                                                                           |
| ---------------- | ------------------------------------------ | -------------------------------------------------------------- | ------------------------------------------------------------------------------------- |
| **A**            | ~**4–9** months                            | ~**3–6** months                                                | Three seniors parallelize validation, sync, tests; fuzzing owned in-house             |
| **A′**           | ~**2–5** months (often overlaps **A**)     | ~**1–3** months                                                | Degradation and limits in parallel with late **A**; extra hire helps load experiments |
| **B**            | ~**10–18** months from **B** kickoff       | ~**8–14** months                                               | Early **4th+** hire on ZK or systems; semi-privacy milestone explicit                 |
| **C**            | ~**4–10** months (includes audit + fixes)  | ~**3–8** months                                                | Pre-booked audit; dedicated in-house fix sprint capacity                              |
| **D**            | ~**4–10** months from **D** kickoff        | ~**4–9** months                                                | In-house client dev; overlap with late **C** if APIs stable                           |
| **E** (optional) | ~**4–10** months                           | ~**4–9** months                                                | Tight product scope; legal retainer; backend/policy in-house parallel to UI           |


**End-to-end (must-haves through one wallet surface, Phases A→D):** with **three seniors** and **in-house-only** engineering, a plausible **aggressive** range is roughly **~16–24 months** from project start, assuming **fast** follow-on hiring and smooth audits. A **conservative** range—allowing hiring slip and audit rounds—is often **~20–30 months**. This compares to **~18–36 months** under a more **gradual** staffing curve.

**Phase E** can add **~4–9+ months** depending on custody scope and jurisdiction; it can **partially overlap** Phase **D** only if signing and policy architecture are fixed early.

### Spend pattern (qualitative)


| Category          | Notes                                                                                                                                                     |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Personnel**     | Sign-on bonuses and **fast** recruiting for **4th+** FTE; fully loaded cost scales with headcount—**no** outsourced dev line item                         |
| **Audits**        | Fees for **external** reviewers; possible **multiple** rounds if consensus and ZK are scoped separately                                                   |
| **Tools & infra** | CI at scale, dedicated testnets, crash analytics, load-generation hardware                                                                                |
| **Risk reserve**  | Budget **10–20%** of engineering time/money for **merge integration**, audit fixes, and schedule slip—smaller than a multi-vendor model, but still needed |


### Risks specific to this model


| Risk                     | Mitigation                                                                                                                      |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------- |
| **Key-person / burnout** | Hire **4th** engineer early; enforce sustainable pace; rotate on-call and review duty                                           |
| **Serial bottleneck**    | Keep **ZK** and **ledger** integration on weekly integration checkpoints with one clear merge owner                             |
| **Audit surprises**      | Internal security + fuzzing from **A** onward; no “first audit is the first review”                                             |
| **Hiring delay**         | If recruiting lags, calendar approaches **baseline** (~18–36 months) even with high budget—**people** are the limiter, not cash |


---

## Indicative resources (engineering FTE-months)

Figures are **cumulative ranges** for serious execution; translate to budget using **fully loaded cost per engineer-month** in your geography. External audits are **additional** line items.


| Component                                     | Indicative range | Notes                                                                                                          |
| --------------------------------------------- | ---------------- | -------------------------------------------------------------------------------------------------------------- |
| Base chain + correctness-first defense        | **5–14**         | Heavy on validation, fuzzing, adversarial tests; includes ingress hardening aligned with fraud prevention      |
| DDoS survival + graceful degradation          | **+2–6**         | Often parallel with A; emphasizes overload behavior, not peak performance under attack                         |
| Encrypted token (engineering-led, new code)   | **35–85**        | Long pole: prover/verifier integration, ledger rules, performance; semi-privacy first may reduce calendar time |
| External ZK/crypto audit                      | —                | Typically **$200k–$800k+** and **2–4 months** calendar, scope-dependent                                        |
| AI wallet MVP                                 | **3–10**         | Excludes legal/compliance-heavy operating model                                                                |
| One wallet platform (mobile **or** extension) | **4–12**         | Second platform **+4–12** FTE-months                                                                           |


**Ongoing (post-MVP):** often **0.25–0.75 FTE** on core node; **0.5–1.0 FTE** if AI wallet product is live and evolving.

**Calendar horizon:** covering must-haves across pillars typically spans **roughly 18–36 months** depending on hiring, audit scheduling, and whether Phases **D**–**E** run in parallel; see **Team plan** for concurrent FTE by phase. For **minimum practical wall-clock time** with **all engineering in-house**, **three seniors at day one**, and **budget** used to accelerate **additional hires**, tools, and audits, see **Minimum calendar time (in-house, three seniors at start)**—indicative **~16–24 months** (aggressive) to **~20–30 months** (conservative) for Phases **A**–**D** must-haves.

---

##  summary

- **Team:** **All product engineering in-house**; planning assumes **three senior employees** at start and growth through further **permanent hires** (see **Starting team** and **Hiring sequence**). Core skills: **systems/C++ + ZK + internal security**, plus **client** for Phase **D** and optional **AI wallet** for Phase **E**.
- **Minimum delivery time:** With **strong budget** used for **fast hiring**, **tools**, and **pre-booked external audits** (not outsourced coding), and a **day-one trio** of seniors, indicative **~16–24 months** (aggressive) to **~20–30 months** (conservative) for **A**–**D** must-haves—still bounded by **audit cycles**, **in-house integration**, and **recruiting speed** (see **Minimum calendar time**).
- **Must-haves:** Correct chain behavior under **fraudulent injection**; **survivable** service under DDoS with **acceptable degradation**; encrypted token delivered as **engineering** with **new code** and **STARK-class quality** aligned with strong OSS references; **one** solid wallet entry point.
- **Nice-to-haves:** Formal methods, sustained performance under simultaneous attack, full privacy at first launch, multi-client parity, highly autonomous AI finance.
- **Budgeting:** Engineer FTE-months × fully loaded rates **plus** discrete **audit** budgets for cryptography and client security.

---

## Document history

- Initial version: aligned with clarified network-defense priorities (correctness → DDoS survival with degradation OK) and encrypted-token engineering assumptions (OSS-comparable STARK quality, original implementation, no code copying).
- Added **Team plan** section: core roles table, planning FTE by phase, hiring sequence, and lean-run options.
- Added **Minimum calendar time** (revised): **in-house-only** engineering, **three seniors at start**; removed contractor/agency model; updated compressed calendar ranges and risks.

