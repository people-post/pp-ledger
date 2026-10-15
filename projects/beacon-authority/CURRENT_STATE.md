# Current state — beacon authority

| Capability | Status | Notes |
|------------|--------|-------|
| Design, part 1 (the real beacon) | **Decided** | [DESIGN.md](DESIGN.md), decisions B001–B010 |
| Genesis trust roots (ba1) | **Yes** | Recovery set + generation-1 certificate in `GenesisAccountMeta` v8 (`ChainAuthority`); `T_CONFIG` and genesis renewals carry it unchanged; `--init` writes / reuses `keys/beacon-signing.{key,pub}` — normative in [WIRE_SCHEMA.md § Chain authority](../../docs/contracts/WIRE_SCHEMA.md#chain-authority), [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md). Next: ba2 |
| Design, part 2 (promotion) | **Outlined** | Three cases agreed; detailed design after part 1 |
| Certificate record / signed tip / generations | **No** | Phases ba2–ba7 |
| Equivocation halt | **No** | Phase ba6 |
| Recovery action (announce, delay, apply) | **No** | Phase ba7 (the set itself ships in ba1) |

Today the beacon is authenticated only by the Amp peer id pinned in its downstream's config (one
hop); see DESIGN § Where we are today.
