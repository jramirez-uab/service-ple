#!/bin/bash
# service-ple: is this table file fast enough for an agent without a server? Measures 4 KB random reads on the file and rows/s through an
# agent alone on it (small cache, random rows: every row comes from the disk). Usage: bench/disco.sh table.safetensors [build]
# Reference (dcc-gr6, NVMe): XFS ~426,000 reads/s; ZFS ~95,000 even with recordsize=4K; ZFS with the default 1 MB records ~3,000.
set -eu
F=$1; B=${2:-build}; T=$(mktemp -d)
trap 'kill $(jobs -p) 2>/dev/null; rm -rf $T' EXIT
$B/service-ple-lecturas "$F" 64 5 1
$B/service-ple-agent --socket $T/a.sock --filas 1024 --fichero "$F" > $T/a.log 2>&1 &
for _ in $(seq 60); do [ -S $T/a.sock ] && break; sleep 1; done
grep AVISO $T/a.log || true
for c in "1 256" "16 256" "4 65536"; do $B/service-ple-carga "unix:$T/a.sock" $c 5; done
