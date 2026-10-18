# Current state — domains

| Capability | Status | Notes |
|------------|--------|-------|
| Design (dn0) | Done | [DESIGN.md](DESIGN.md); DN001–DN006: symbols as domains for system (1–3 letters, genesis) and issued accounts (4–7, registrar), read-only while the account lives, free again when it ends (first built as retired; revised), proven both ways under the state root |
| State and rules (dn1) | Done | `Wallet.domain`; `Domains.h` (form, tree key, leaf); `AccountBuffer` domain index and leaves, freed on removal; `T_DOMAIN_ASSIGN` (7, `DomainAssignTxHandler`); `T_NEW_USER` with a domain (the registrar's, or the genesis block's); account ids below 2⁶³; genesis `SystemNames`; tests `TheRegistrarGivesIssuedAccountsDomainsProvenBothWays`, `DomainTest` |
| Proofs, pp-client (dn2) | Done | `DOMAIN_GET` (2101) with `DomainProof`, `verify::checkDomainProof`; kind `domain-assign` (`op::assignDomain`); `pp-client domain`, `verified-domain`, `op new-account --domain`, `op assign-domain`; init-config `systemNames`; L1 smoke `run_domain_scenario` |
| pp-ledger-apps (dn3) | No | |
| Docs (dn4) | No | |
