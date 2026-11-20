# Operation file

A transaction that several people sign, carried between them as a file: what it does, who must sign
it, the signatures collected so far. Normative. The record it carries and its signing message are
[WIRE_SCHEMA.md](WIRE_SCHEMA.md)'s; the library is `pp_operation` (`include/ledger/operation/Operation.h`),
the commands `pp-client op …` (§ Commands). The desktop apps (pp-ledger-apps) read and write the same
files.

## Flow

```text
create (op transfer | mint | new-account | wrap) ──► copy to each signer
   each signer: op show, op sign  (own copy, in parallel, offline)
   op merge copies ──► op submit (needs the quorum) ──► relay or beacon
```

Nobody waits for another signer: each signs their own copy, and any copies of one operation merge.

## Format

UTF-8 JSON, one object. Byte strings are base64 (RFC 4648, with padding), except `genesisHash`
(hex).

| Field | Type | Holds |
|-------|------|-------|
| `format` | string | `pp-ledger-operation` |
| `version` | integer | `1`; a reader refuses any other |
| `networkId` | string | The chain's network id: part of the signing message |
| `genesisHash` | hex string | Block 0's hash (32 bytes), or `""` when the creator did not know it |
| `kind` | string | `transfer`, `batch`, `mint`, `new-account`, `account-update`, `account-recovery`, `domain-assign`, `config-update`, `beacon-certificate`, `recovery`, `genesis`, or `record` (any other type) |
| `record` | object | `type` (the record type, integer) and `data` (base64 of the packed payload): what is signed |
| `signers` | object | `account` (integer; absent for a key set), `minSignatures` (integer ≥ 1), `publicKeys` (base64 ML-DSA-65 public keys) |
| `signatures` | array | Base64 ML-DSA-65 signatures of `record` collected, in any order |
| `moreRecords` | array | A `genesis`'s (§ Genesis): block 0's further records; a `batch`'s (§ Batch): its other transfers. Each `{type, data, signatures}` |
| `notes` | string | The creator's free text |
| `createdAt` | integer | Unix seconds when the operation was made |

The signed bytes are `record.signingMessage(networkId)` ([WIRE_SCHEMA.md § Signing message](WIRE_SCHEMA.md#signing-message)):
a signature in the file is exactly a signature the node checks.

## Rules

- **Read.** A reader refuses a file whose `format` or `version` it does not know; whose `record`
  does not decode as its type; whose `kind` is not the record's (§ Kinds); whose keys are not
  ML-DSA-65 public keys; that needs more signatures than it lists keys; or whose `signers.account`
  is not the account the chain checks the record against (§ Kinds). Signatures are not checked on
  reading.
- **Meaning comes from the record.** What a tool shows of an operation is computed from `record`
  every time; `kind` is a check, and `notes` are shown as the creator's words, never as what the
  operation does.
- **Signatures.** A signature counts when it verifies under one of `signers.publicKeys` and that
  key has not been counted already; others are invalid (shown, never submitted). Only a signer
  signs, and each key once. With `moreRecords`, a key has signed once it has signed every record.
- **Merge.** Copies merge when everything but the signatures is identical; the result keeps the
  valid signatures of each record, one per key, in `publicKeys` order.
- **Submit.** Only with `minSignatures` valid signatures; the node's network id must be
  `networkId`, and its block 0 hash `genesisHash` when one is named. The submitted record carries
  the valid signatures only; a batch's records are submitted one by one. Submitting again is harmless (the pool keeps one copy; the chain one
  transaction per payer and idempotent id).
- **Validity.** The record's validity window (`validationTsMin`…`validationTsMax`) is the deadline
  for every signer; the chain caps its length (`maxValidationTimespanSeconds`, default one day).
  An expired operation is made again: a change to the record is a new operation.

## Kinds

| `kind` | Record | Signed by (`signers.account`) |
|--------|--------|-------------------------------|
| `mint` | `T_DEFAULT` from genesis (0) to the reserve (1): the only way to mint | The genesis (admin) account, 0 |
| `transfer` | Any other `T_DEFAULT` | Its `fromWalletId` |
| `batch` | Two or more transfers (`record`, then `moreRecords`) from one account | Their `fromWalletId` |
| `new-account` | `T_NEW_USER` | Its `fromWalletId` |
| `account-update` | `T_USER_UPDATE` | Its `walletId`: the keys in force sign for the keys and threshold that replace them (and the first recovery set) |
| `domain-assign` | `T_DOMAIN_ASSIGN` | The registrar, 2 ([WIRE_SCHEMA § Domains](WIRE_SCHEMA.md#domains)) |
| `account-recovery` | `T_ACCOUNT_RECOVERY` | Its `walletId`, but its keys are the account's **recovery set** and its threshold ([WIRE_SCHEMA § Account recovery](WIRE_SCHEMA.md#account-recovery)) |
| `config-update` | `T_CONFIG` | The genesis account, 0 |
| `beacon-certificate` | `T_BEACON_CERT` | The genesis account, 0 |
| `recovery` | `T_RECOVERY` | The recovery key set ([BEACON_AUTHORITY.md](BEACON_AUTHORITY.md)): no `account` |
| `genesis` | `T_GENESIS` | The genesis account, 0 |
| `record` | Any other type | Not checked |

## Batch

Several transfers from one account, reviewed and signed together (pp-ledger-apps' fee app pays a
round's recipients this way).

- `record` is the first transfer, `moreRecords` the others: 2 to 1,000 (`kMaxBatchTransfers`), each
  a `T_DEFAULT` from `signers.account` (not a mint), no two the same. Each has its own idempotent id;
  they share the validity window, the fee (each pays it) and, when given, the metadata.
- A key signs every transfer at once; the summary is the table — each recipient and amount, the
  totals per token, the fees.
- Each transfer is its own transaction: submitted one by one, admitted and applied on its own (the
  pool takes up to 1,000 pending per account), possibly in different blocks. A transfer the
  account cannot cover when it applies is refused alone; the creator checks the total against the
  balance.

## Genesis

Block 0 signed offline: the genesis keys never reach the beacon host.

- `record` is the genesis record (`T_GENESIS`); `moreRecords` the rest of block 0, in order — the
  fee, reserve, registrar and recycle accounts, then the genesis miners (`T_NEW_USER` from the
  genesis account, each). A reader refuses `moreRecords` in any other kind, and any record there
  but such an account.
- `networkId` and `signers` (account 0, the admin keys and threshold) are the genesis record's;
  `genesisHash` is empty (there is no block 0 yet).
- The genesis record's config names the network's start (`genesisTime`): nodes stay at slot 0
  until then. A late start is harmless (missed slots are charged within the first block's epoch
  only, as when miners come up after any start).
- `pp-beacon -d DIR --plan-genesis FILE [--start T] [--notes TEXT]` writes the plan from `DIR`'s
  `init-config.json`, certifying `DIR`'s beacon signing key (made if missing). `--start`: Unix
  seconds or `+N` seconds from now; default now.
- `pp-beacon -d DIR --init --genesis-op FILE` writes block 0 from a complete genesis operation; it
  refuses one certifying another beacon key, and one short of its quorum. The chain checks every
  record's signatures as it would those of a key-signed genesis.
- A re-genesis (`--import-state`) is still signed with `--genesis-key` on the beacon host.

## Commands

`pp-client op …` ([SETUP.md § Client](../ops/SETUP.md#client)):

| Command | Does | Node |
|---------|------|------|
| `op transfer --from A --to B --amount N -f FEE -o FILE` | A transfer from A | When not given `--network-id` and `--signer-key` |
| `op mint --amount N -f FEE -o FILE` | A mint (genesis → reserve) | Same |
| `op new-account --from A --id ID --account-key K… --amount N [--domain NAME] -f FEE -o FILE` | A new account, funded by A; an issued one may carry its domain (from the registrar) | Same |
| `op assign-domain --account A --domain NAME -f FEE -o FILE` | Issued account A's domain (4–7 letters, once), signed by the registrar | Same |
| `op update-keys --account A --new-key K… [--new-min-signatures N] -f FEE -o FILE` | A's keys and threshold replaced (default: all new keys); the rest of A's meta as the node has it | Always |
| `op set-recovery --account A (--recovery-key K… [--recovery-min-signatures N] --delay-seconds S \| --none) -f FEE -o FILE` | A's recovery set (N of the keys, default 1) and delay: named by A's keys when A has none; after that a change signed by A's recovery set (`--none` removes it) | Always |
| `op recover --account A --new-key K… [--new-min-signatures N] -f FEE -o FILE` | A's recovery set announces A's next keys; A is frozen until they take over (the first epoch start after A's delay); A pays the fee | When not given `--network-id` and `--signer-key` |
| `op withdraw-recovery --account A -f FEE -o FILE` | A's recovery set withdraws the recovery announced: A's keys sign again | Same |
| `op batch --from A --payments CSV [--meta TEXT] -f FEE -o FILE` | A batch from A: a transfer per CSV line `to,amount[,token]` (`#` comments), each paying FEE and carrying TEXT | When not given `--network-id` and `--signer-key` |
| `op wrap TX-FILE -o FILE` | An operation of a record made by `mk-beacon-cert`, `mk-recovery`, `mk-tx`, … (its signatures dropped) | Same; a recovery always needs `--signer-key` |
| `op show FILE` | What it does, its deadline, who signed (key fingerprints), whether complete | No |
| `op sign FILE -k KEY [-o OUT]` | Add a signature (replaces FILE unless `-o`) | No |
| `op merge FILE… -o OUT` | Merge signed copies | No |
| `op submit FILE` | Submit a complete operation (`-b --host <relay or beacon>`); a batch transfer by transfer, reporting any refused; not a genesis (§ Genesis) | Yes |

Creators take `--valid-for SECONDS` (default 86400; the window opens a minute before now),
`--notes`, and, offline, `--network-id`, `--genesis-hash`, `--signer-key` (repeat) and
`--min-signatures` (default: all keys). With a node they read the network id, block 0's hash and
the signing account's keys and threshold from it (the account's recovery set for what that set signs).

A key's **fingerprint** is the first 16 hex digits of its SHA-256, in groups of four
(`29ed-2bc7-b246-1936`): what signers compare.
