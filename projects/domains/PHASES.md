# Domains — phases

Ordering only; status lives in [CURRENT_STATE.md](CURRENT_STATE.md). Design: [DESIGN.md](DESIGN.md).

- [x] **dn0** Design: symbols as domains, read-only, ending with their account, proven both ways (this folder)
- [x] **dn1** The state and rules: `Wallet.domain`, domain leaves under the state root, account ids below 2⁶³; genesis names; `T_NEW_USER` with a domain; `T_DOMAIN_ASSIGN`; freed when the account ends; unit tests
- [x] **dn2** Proofs and pp-client: `DOMAIN_GET` with its proof, `verify::checkDomainProof`; `pp-client domain`, `verified-domain`, `op new-account --domain`, `op assign-domain`; genesis init names; L1 smoke
- [x] **dn3** pp-ledger-apps: the registrar's domains; symbols in the wallet and token app, proven; send by symbol; the genesis plan's names; chain smoke
- [x] **dn4** Docs: WIRE_SCHEMA § Domains, NAME_DIRECTORY, OPERATION_FILE; the guides (the apps', with dn3)
