# Operation file

A transaction that several people sign, carried between them as a file: what it does, who must sign
it, the signatures collected so far. Normative. The record it carries and its signing message are
[WIRE_SCHEMA.md](WIRE_SCHEMA.md)'s; the library is `pp_operation` (`src/operation/Operation.h`),
the commands `pp-client op …` (§ Commands). The admin apps (pp-ledger-admin) read and write the same
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
| `kind` | string | `transfer`, `mint`, `new-account`, `config-update`, `beacon-certificate`, `recovery`, `genesis`, or `record` (any other type) |
| `record` | object | `type` (the record type, integer) and `data` (base64 of the packed payload): what is signed |
| `signers` | object | `account` (integer; absent for a key set), `minSignatures` (integer ≥ 1), `publicKeys` (base64 ML-DSA-65 public keys) |
| `signatures` | array | Base64 ML-DSA-65 signatures collected, in any order |
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
  signs, and each key once.
- **Merge.** Copies merge when everything but `signatures` is identical; the result keeps the valid
  signatures, one per key, in `publicKeys` order.
- **Submit.** Only with `minSignatures` valid signatures; the node's network id must be
  `networkId`, and its block 0 hash `genesisHash` when one is named. The submitted record carries
  the valid signatures only. Submitting again is harmless (the pool keeps one copy; the chain one
  transaction per payer and idempotent id).
- **Validity.** The record's validity window (`validationTsMin`…`validationTsMax`) is the deadline
  for every signer; the chain caps its length (`maxValidationTimespanSeconds`, default one day).
  An expired operation is made again: a change to the record is a new operation.

## Kinds

| `kind` | Record | Signed by (`signers.account`) |
|--------|--------|-------------------------------|
| `mint` | `T_DEFAULT` from genesis (0) to the reserve (1): the only way to mint | The genesis (admin) account, 0 |
| `transfer` | Any other `T_DEFAULT` | Its `fromWalletId` |
| `new-account` | `T_NEW_USER` | Its `fromWalletId` |
| `config-update` | `T_CONFIG` | The genesis account, 0 |
| `beacon-certificate` | `T_BEACON_CERT` | The genesis account, 0 |
| `recovery` | `T_RECOVERY` | The recovery key set ([BEACON_AUTHORITY.md](BEACON_AUTHORITY.md)): no `account` |
| `genesis` | `T_GENESIS` | The genesis account, 0 |
| `record` | Any other type | Not checked |

## Commands

`pp-client op …` ([SETUP.md § Client](../ops/SETUP.md#client)):

| Command | Does | Node |
|---------|------|------|
| `op transfer --from A --to B --amount N -f FEE -o FILE` | A transfer from A | When not given `--network-id` and `--signer-key` |
| `op mint --amount N -f FEE -o FILE` | A mint (genesis → reserve) | Same |
| `op new-account --from A --id ID --account-key K… --amount N -f FEE -o FILE` | A new account, funded by A | Same |
| `op wrap TX-FILE -o FILE` | An operation of a record made by `mk-beacon-cert`, `mk-recovery`, `mk-tx`, … (its signatures dropped) | Same; a recovery always needs `--signer-key` |
| `op show FILE` | What it does, its deadline, who signed (key fingerprints), whether complete | No |
| `op sign FILE -k KEY [-o OUT]` | Add a signature (replaces FILE unless `-o`) | No |
| `op merge FILE… -o OUT` | Merge signed copies | No |
| `op submit FILE` | Submit a complete operation (`-b --host <relay or beacon>`) | Yes |

Creators take `--valid-for SECONDS` (default 86400; the window opens a minute before now),
`--notes`, and, offline, `--network-id`, `--genesis-hash`, `--signer-key` (repeat) and
`--min-signatures` (default: all keys). With a node they read the network id, block 0's hash and
the signing account's keys and threshold from it.

A key's **fingerprint** is the first 16 hex digits of its SHA-256, in groups of four
(`29ed-2bc7-b246-1936`): what signers compare.
