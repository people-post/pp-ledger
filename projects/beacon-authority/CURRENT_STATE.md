# Current state — beacon authority

| Capability | Status | Notes |
|------------|--------|-------|
| Design, part 1 (the real beacon) | **Decided** | [DESIGN.md](DESIGN.md), decisions B001–B013 |
| Genesis trust roots (ba1) | **Yes** | Recovery set + generation-1 certificate in `GenesisAccountMeta` v8 (`ChainAuthority`); `T_CONFIG` and genesis renewals carry it unchanged; `--init` writes / reuses `keys/beacon-signing.{key,pub}` — normative in [WIRE_SCHEMA.md § Chain authority](../../docs/contracts/WIRE_SCHEMA.md#chain-authority), [CONFIGURATION.md](../../docs/ops/CONFIGURATION.md) |
| Certificate record (ba2) | **Yes** | `T_BEACON_CERT` (id 18): admin set certifies generation + 1, in force when its block applies, one per block; out-of-band check `Chain::verifyBeaconCertificateRecord` (any higher generation); `pp-client mk-beacon-cert` → `sign-tx` → `submit-tx` — normative in [WIRE_SCHEMA.md § Chain authority](../../docs/contracts/WIRE_SCHEMA.md#chain-authority). Nothing reads the certificate yet (ba4–ba5) |
| Signed tip (ba3) | **Yes** | The beacon signs every committed block, persists the latest in `data/signed-tip.dat` before serving it, refuses a ledger behind or beside it; `STATUS` / `BLOCK_WAIT` / `BLOCK_ADD` carry it; relays keep the newest and pass it on — normative in [WIRE_SCHEMA.md § Signed tip](../../docs/contracts/WIRE_SCHEMA.md#signed-tip). |
| Adoption rule (ba4) | **Yes** | Sync commits only up to a verified signed tip, after checking the hash chain to it; a miner commits its own block, and a relay a forwarded one, only on a `BLOCK_ADD` reply whose verified tip covers it; relays keep only tips that verify — [WIRE_SCHEMA.md § Signed tip](../../docs/contracts/WIRE_SCHEMA.md#signed-tip). Tips of another generation do not verify yet (ba5). Next: ba5 |
| Open: fresh node's trust root | **Gap** | A node with no blocks takes its first block from its upstream on trust ([B013](DECISIONS.md#b013--adoption-authenticate-the-range-then-apply-first-block-on-trust)); `networkAnchor.genesisHash` pins genesis for miners but not a checkpoint start. Planned with ba5 |
| Design, part 2 (promotion) | **Outlined** | Three cases agreed; detailed design after part 1 |
| Generations | **No** | Phase ba5 |
| Equivocation halt | **No** | Phase ba6 |
| Recovery action (announce, delay, apply) | **No** | Phase ba7 (the set itself ships in ba1) |

Today the beacon is authenticated only by the Amp peer id pinned in its downstream's config (one
hop); see DESIGN § Where we are today.
