#!/usr/bin/env bash
#
# Publish one normalized resource snapshot through External API
# (DESIGN.md §5.2, §5.4).
#
#   SLATE_API_KEY=... tools/direct/publish.sh <host> [snapshot.json]
#
# The snapshot is read from a file or from stdin and is §5.2's document without
# `provider`, which the endpoint fixes to `direct` — a body that could name its
# own provider is a body that can write into the Home Assistant half of the
# store. With no argument the example below is sent, which is the shortest way
# to see a panel accept state:
#
#   SLATE_API_KEY=... tools/direct/publish.sh 192.168.1.42
#
#   {"resource":"living-room","kind":"light","name":"Living room",
#    "available":true,"state":{"power":"on","brightness":62},
#    "capabilities":{"toggle":true,"set_brightness":{"min":0,"max":100}}}
#
# This is half of the round trip. `agent.py` beside it is the other half: it
# attaches over the WebSocket, answers the `action` frames a tap produces, and
# publishes the resulting state back through this same endpoint.
#
# The refusals are §5.4's and they are worth reading rather than retrying. In
# particular `404 resource_not_bound` is not a permission error and not a typo
# in the URL: the store holds only what the active configuration references
# (§5.1), so publishing an id no tile binds has nowhere to go. That is also the
# bound that stops a LAN client filling PSRAM, which is why it is a refusal and
# not a silent accept.
#
# The scoped credential comes from $SLATE_API_KEY and is handed to curl through
# a config file on stdin rather than as an argument, so it does not appear in
# the process list of a shared machine. The editor creates named keys and shows
# each plaintext once. $SLATE_TOKEN remains a backwards-compatible development
# fallback for panels or scripts predating named integration keys.

set -euo pipefail

usage() {
    cat >&2 <<'EOF'
Publish a normalized resource snapshot through External API (DESIGN.md §5.4).

  SLATE_API_KEY=... tools/direct/publish.sh <host> [snapshot.json]

  host       an address, a name, or a full URL: 192.168.1.42,
             slate-a1b2c3.local, http://192.168.1.42
  snapshot   a §5.2 snapshot without `provider`. `-` reads stdin. Omitted
             sends the living-room light of the example in this script.
EOF
    exit 2
}

[[ $# -ge 1 && $# -le 2 ]] || usage

HOST="$1"
SOURCE="${2:-}"
AUTH_TOKEN="${SLATE_API_KEY:-${SLATE_TOKEN:-}}"
[[ -n "${AUTH_TOKEN}" ]] || {
    echo "set SLATE_API_KEY to a named External API key" >&2
    exit 2
}

case "${HOST}" in
    http://*|https://*) BASE="${HOST}" ;;
    *)                  BASE="http://${HOST}" ;;
esac

read -r -d '' EXAMPLE <<'EOF' || true
{"resource":"living-room","kind":"light","name":"Living room","area":"Downstairs",
 "available":true,"state":{"power":"on","brightness":62},
 "capabilities":{"toggle":true,"set_power":true,"set_brightness":{"min":0,"max":100}}}
EOF

case "${SOURCE}" in
    "")  BODY="${EXAMPLE}" ;;
    -)   BODY="$(cat)" ;;
    *)   [[ -f "${SOURCE}" ]] || { echo "no snapshot at ${SOURCE}" >&2; exit 1; }
         BODY="$(cat "${SOURCE}")" ;;
esac

response="$(mktemp)"
trap 'rm -f "${response}"' EXIT

status="$(printf 'header = "Authorization: Bearer %s"\n' "${AUTH_TOKEN}" |
    curl --config - \
        --silent \
        --request POST \
        --header 'Content-Type: application/json' \
        --data-binary "${BODY}" \
        --connect-timeout 5 \
        --max-time 15 \
        --output "${response}" \
        --write-out '%{http_code}' \
        "${BASE}/api/v1/direct/state")"

echo "HTTP ${status} $(cat "${response}")"

# 202 is the whole of §5.4's success: the snapshot is accepted and enqueued, and
# the tile that renders it is rebuilt on the UI task, which has not necessarily
# run yet. Anything else is a refusal that left the last confirmed value alone.
[[ "${status}" == "202" ]]
