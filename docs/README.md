# Documentation map

pp-ledger docs are split by **stability and blast radius**, similar in spirit to
pp-browser’s `docs/` tiers.

| Tier | Folder | Change bar | Purpose |
|------|--------|------------|---------|
| **Architecture** | [`architecture/`](architecture/) | Rare; wide consequences | Roles, topology, testing doctrine, server shape, open consensus items |
| **Contracts** | [`contracts/`](contracts/) | Versioned; additive preferred | Wire formats, Amp transport, on-disk storage |
| **Product** | [`product/`](product/) | Evolve with product | Vision / design overview, name directory, encrypted-token extension |
| **Ops** | [`ops/`](ops/) | Freely | Setup, CI, test strategy, budget/roadmap |
| **Print** | [`print/`](print/) | Assets | Diagrams, print CSS, HTML exports |

**`docs/` = what the system is.** In-tree module notes (`src/consensus/README.md`,
`src/server/SERVER.md` stub) point here for the canonical copy.

Agent entry: [`AGENTS.md`](../AGENTS.md). Deploy howto: [`deploy/README.md`](../deploy/README.md).

---

## Architecture

| Doc | Topic |
|-----|--------|
| [architecture/LEDGER_TOPOLOGY.md](architecture/LEDGER_TOPOLOGY.md) | Beacon / relay / miner topology, RPC, sync, STATUS |
| [architecture/PLATFORM_INTEGRATION.md](architecture/PLATFORM_INTEGRATION.md) | Cross-repo (pp-browser, pp-node, Amp, role matrix) |
| [architecture/SERVER.md](architecture/SERVER.md) | Server components, APIs, usage (canonical; formerly `src/server/SERVER.md`) |
| [architecture/FOLDER_DEPENDENCY.md](architecture/FOLDER_DEPENDENCY.md) | Source folder dependency graph |
| [architecture/TESTING.md](architecture/TESTING.md) | Testing doctrine (tiers, push-down, skip taxonomy) |
| [architecture/SLOT_COMMITTEE_OPEN_ITEMS.md](architecture/SLOT_COMMITTEE_OPEN_ITEMS.md) | Deferred SlotCommittee product decisions |

---

## Contracts (normative)

| Doc | Topic |
|-----|--------|
| [contracts/WIRE_SCHEMA.md](contracts/WIRE_SCHEMA.md) | Block/record layouts, **SlotCommittee** leader election |
| [contracts/AMP_TRANSPORT.md](contracts/AMP_TRANSPORT.md) | Amp / ADP multiaddrs, ledger RPC over Amp |
| [contracts/LEDGER_STORAGE.md](contracts/LEDGER_STORAGE.md) | On-disk volumes / indexes |

---

## Product

| Doc | Topic |
|-----|--------|
| [product/DESIGN.md](product/DESIGN.md) | Product design overview (EN) |
| [product/DESIGN.zh.md](product/DESIGN.zh.md) | Product design overview (ZH) |
| [product/NAME_DIRECTORY.md](product/NAME_DIRECTORY.md) | Domains / memorable names (schema; not fully live) |
| [product/ENCRYPTED_TOKEN.md](product/ENCRYPTED_TOKEN.md) | Encrypted-token extension (EN) |
| [product/ENCRYPTED_TOKEN.zh.md](product/ENCRYPTED_TOKEN.zh.md) | Encrypted-token extension (ZH) |

---

## Ops

| Doc | Topic |
|-----|--------|
| [ops/SETUP.md](ops/SETUP.md) | Local beacon / relay / miner / client setup |
| [ops/TEST_STRATEGY.md](ops/TEST_STRATEGY.md) | Purpose IDs (`L-*`), smoke driver, CI ladder |
| [ops/GITHUB_ACTIONS.md](ops/GITHUB_ACTIONS.md) | CI / Actions setup |
| [ops/DEVELOPMENT_BUDGET.md](ops/DEVELOPMENT_BUDGET.md) | Roadmap / budget planning |

Smoke scripts: `scripts/test/pp_ledger_local_test.sh`. Image compose: `deploy/`.

---

## Print / exports

See [print/README.md](print/README.md). Pre-built HTML under [print/html/](print/html/).
