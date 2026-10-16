#!/usr/bin/env bash
# One-time setup of the sample stack (deploy/compose.yml): generates every key in secrets/, the
# genesis config, and .env with the nodes' peer ids (worked out from their identity keys, so no
# multiaddr is copied by hand), then writes genesis. Run again only after removing secrets/, .env
# and the stack's volumes (`docker compose down -v`).
#
# A sample: in production the genesis, recovery and admin keys are made and kept offline by their
# holders, and node keys come from a secret manager (PP_*_FILE). See deploy/README.md.
#
# Environment: PP_LEDGER_IMAGE, PP_LEDGER_NETWORK_ID (local-devnet), PP_LEDGER_SLOT_DURATION (7),
# PP_LEDGER_SLOTS_PER_EPOCH (86400), MINER_ID (1048576, the genesis miner).
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=${PP_LEDGER_IMAGE:-ghcr.io/people-post/pp-ledger:latest}
NETWORK_ID=${PP_LEDGER_NETWORK_ID:-local-devnet}
SLOT_DURATION=${PP_LEDGER_SLOT_DURATION:-7}
SLOTS_PER_EPOCH=${PP_LEDGER_SLOTS_PER_EPOCH:-86400}
MINER_ID=${MINER_ID:-1048576}

die() { echo "error: $*" >&2; exit 1; }
[[ -e secrets || -e .env ]] && die "secrets/ or .env exists: this stack is set up already"

mkdir -m 700 secrets
# The image's pp-client, as this user, writing into secrets/.
client() { docker run --rm -u "$(id -u):$(id -g)" -v "$PWD/secrets:/out" -w /out "$IMAGE" pp-client "$@"; }
for name in genesis1 genesis2 genesis3 reserve registrar fee recycle recovery \
            beacon-signing beacon-identity relay-identity miner; do
  client keygen -o "/out/${name}" >/dev/null
done
# Secrets are bind-mounted with these permissions: readable by the container user; secrets/ (0700)
# keeps them private on the host.
chmod 644 secrets/*.key secrets/*.pub

pub() { tr -d ' \n' <"secrets/$1.pub"; }
peer_id() { client peer-id "/out/$1.key" | sed -n 's/^Peer id: *//p'; }

cat >secrets/init-config.json <<JSON
{
  "networkId": "${NETWORK_ID}",
  "slotDuration": ${SLOT_DURATION},
  "slotsPerEpoch": ${SLOTS_PER_EPOCH},
  "systemAccounts": {
    "genesis": {"publicKeys": ["$(pub genesis1)", "$(pub genesis2)", "$(pub genesis3)"], "minSignatures": 2},
    "reserve": {"publicKeys": ["$(pub reserve)"]},
    "registrar": {"publicKeys": ["$(pub registrar)"]},
    "fee": {"publicKeys": ["$(pub fee)"]},
    "recycle": {"publicKeys": ["$(pub recycle)"]}
  },
  "recovery": {"publicKeys": ["$(pub recovery)"]},
  "genesisMiners": [{"id": ${MINER_ID}, "publicKeys": ["$(pub miner)"]}]
}
JSON
chmod 644 secrets/init-config.json

cat >.env <<ENV
PP_LEDGER_IMAGE=${IMAGE}
MINER_ID=${MINER_ID}
BEACON_PEER_ID=$(peer_id beacon-identity)
RELAY_PEER_ID=$(peer_id relay-identity)
MINER_PEER_ID=$(peer_id miner)
ENV

docker compose --profile init run --rm beacon-init
echo
echo "Genesis written (network ${NETWORK_ID}). Move secrets/genesis*.key and secrets/recovery.key"
echo "offline, then start the stack: docker compose up -d"
