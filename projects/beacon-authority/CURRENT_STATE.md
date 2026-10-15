# Current state — beacon authority

| Capability | Status | Notes |
|------------|--------|-------|
| Design, part 1 (the real beacon) | **Decided** | [DESIGN.md](DESIGN.md), decisions B001–B011 |
| Genesis trust roots (ba1) | **Yes** | Recovery set + generation-1 certificate in `GenesisAccountMeta` v8 (`ChainAuthority`); `T_CONFIG` and genesis renewals carry it unchanged; `--init` writes / reuses `keys/beacon-signing.{key,pub}` — normative in [WIRE_SCHEMA.md § Chain authority](../../docs/contracts/WIRE_SCHEMA.md#chain-authority), [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md) |
| Certificate record (ba2) | **Yes** | `T_BEACON_CERT` (id 18): admin set certifies generation + 1, in force when its block applies, one per block; out-of-band check `Chain::verifyBeaconCertificateRecord` (any higher generation); `pp-client mk-beacon-cert` → `sign-tx` → `submit-tx` — normative in [WIRE_SCHEMA.md § Chain authority](../../docs/contracts/WIRE_SCHEMA.md#chain-authority). Nothing reads the certificate yet (ba3–ba5). Next: ba3 |
| Design, part 2 (promotion) | **Outlined** | Three cases agreed; detailed design after part 1 |
| Signed tip / adoption / generations | **No** | Phases ba3–ba5 |
| Equivocation halt | **No** | Phase ba6 |
| Recovery action (announce, delay, apply) | **No** | Phase ba7 (the set itself ships in ba1) |

Today the beacon is authenticated only by the Amp peer id pinned in its downstream's config (one
hop); see DESIGN § Where we are today.
