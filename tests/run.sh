#!/bin/bash
# Local test: synthetic table, server and agent (ideally built with -DSPLE_SANITIZE=ON); byte-exact rows through the server, through the agent
# (with evictions), the fallback to the server when the agent is down, and the agent alone on the local file.
set -eu
B=${1:-build}; T=$(mktemp -d); trap 'kill $(jobs -p) 2>/dev/null; rm -rf $T; rm -f /dev/shm/service-ple-4bd43c9068f6c2f5' EXIT
python3 "$(dirname "$0")/sintetica.py" $T/t.safetensors
$B/service-ple-server --fichero $T/t.safetensors --puerto 18499 > $T/s.log 2>&1 & sleep 1
P="$B/service-ple-test-prueba --fichero $T/t.safetensors --comprobar 8000"
$P --servidor 127.0.0.1:18499 | tail -1
$B/service-ple-agent --servidor 127.0.0.1:18499 --socket $T/a.sock --filas 1024 > $T/a.log 2>&1 & A=$!; sleep 2
$P --servidor "unix:$T/a.sock;127.0.0.1:18499" | tail -1
kill $A; wait $A 2>/dev/null || true
$P --servidor "unix:$T/a.sock;127.0.0.1:18499" | tail -1
$B/service-ple-agent --socket $T/b.sock --filas 1024 --fichero $T/t.safetensors > $T/b.log 2>&1 & sleep 2
$P --servidor "unix:$T/b.sock" | tail -1
