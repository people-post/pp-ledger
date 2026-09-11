# Amp-aligned smoke / image fixtures
#
# Editable templates for the local harness live under
# `scripts/test/fixtures/*.tmpl` (rendered by `pp_ledger_smoke_lib.sh`).
#
# JSON samples in this directory are for `deploy/compose.smoke.yml`. Replace
# `REPLACE_*_PEER_ID` with peer IDs from each container's
# `AMP ledger listener:` log line after first start.
#
# Dogfood compose (`../compose.yml` + `../configs/{beacon,relay,miner}`) still
# uses sample ports 8517+; prefer ADP multiaddrs (string or `{host,port,peerId}`)
# — not bare `host:port` — for peer dialing.
