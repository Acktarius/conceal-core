#!/bin/sh
# Reproducible two-daemon check for AI provider discovery.
# This script was not executed for the protocol v1 field change.
# IntegrationTestLib can start in-process nodes, but those nodes leave
# discovery disabled and do not expose POST /registeraiprovider.
#
# Build first, from an out-of-source directory:
#   cmake -DBUILD_TESTS=ON ..
#   cmake --build . --target conceal-daemon -j"$(nproc)"
#
# Then point DAEMON at that binary and run this script on a machine that
# can bind the ports below. It does not mine and it does not prove that
# blockchain sync still works beside a discovery-disabled peer. That check
# needs a chain and a third daemon started without --enable-ai-discovery.

set -eu

DAEMON="${DAEMON:-./src/conceal-daemon}"
ROOT="${ROOT:-/tmp/conceal-ai-discovery}"
A="$ROOT/a"
B="$ROOT/b"

rm -rf "$ROOT"
mkdir -p "$A" "$B"

"$DAEMON" --data-dir "$A" --p2p-bind-ip 127.0.0.1 --p2p-bind-port 25000 \
  --rpc-bind-ip 127.0.0.1 --rpc-bind-port 26000 --enable-ai-discovery \
  --log-file "$A/daemon.log" --no-console >"$A/stdout.log" 2>&1 &
echo $! >"$A/pid"

"$DAEMON" --data-dir "$B" --p2p-bind-ip 127.0.0.1 --p2p-bind-port 25001 \
  --rpc-bind-ip 127.0.0.1 --rpc-bind-port 26001 --enable-ai-discovery \
  --add-priority-node 127.0.0.1:25000 \
  --log-file "$B/daemon.log" --no-console >"$B/stdout.log" 2>&1 &
echo $! >"$B/pid"

cleanup() {
  kill "$(cat "$A/pid")" "$(cat "$B/pid")" 2>/dev/null || true
}
trap cleanup EXIT

echo "Wait until both RPC ports answer, then:"
echo "  curl -s http://127.0.0.1:26000/getinfo"
echo "  curl -s http://127.0.0.1:26001/getinfo"
echo "Register the locked advertisement from docs/ai-provider-discovery.md on A."
echo "Query B with POST /getaiproviders until ai_discovery_peers is non-zero and the record appears."
echo "Register sequence 2 with status busy and confirm B replaced the record."
echo "Replay sequence 1 and confirm B still shows busy."
echo "Stop refreshing and confirm the record is gone after expiresAt."
echo "Register more than 64 providers on A and confirm B's ai_provider_count reaches that number across more than one sync round."
echo "Start a third daemon without --enable-ai-discovery and confirm it still exchanges blocks."
