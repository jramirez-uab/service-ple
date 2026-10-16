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
# the agent alone under load (16 connections, random rows, cache of 1024): rows still byte-exact and no request fails
$B/service-ple-carga "unix:$T/b.sock" 16 256 4 > $T/carga.out 2>&1 & L=$!
v=$($P --servidor "unix:$T/b.sock" | tail -1); wait $L || true
[ "$v" = "RESULTADO: OK" ] && grep -q "errores 0$" $T/carga.out && echo "RESULTADO: OK" || echo "RESULTADO: FALLO (agente solo con carga: $v; $(cat $T/carga.out))"
# the engine starts before the agent (falls back to the server) and must move to the agent when it appears (about 35 s)
$B/service-ple-test-preferido "unix:$T/c.sock;127.0.0.1:18499" 36 > $T/p.out 2>&1 & Q=$!; sleep 2
$B/service-ple-agent --servidor 127.0.0.1:18499 --socket $T/c.sock --filas 1024 > $T/c.log 2>&1 & C=$!
wait $Q || true; tail -1 $T/p.out; kill $C; wait $C 2>/dev/null || true
$B/service-ple-test-robustez $T | tail -1
# no threads left (RLIMIT_NPROC soft limit 1): the server and the agent refuse that connection and keep running
$B/service-ple-agent --socket $T/d.sock --filas 1024 --fichero $T/t.safetensors > $T/d.log 2>&1 & D=$!
$B/service-ple-server --fichero $T/t.safetensors --puerto 18498 > $T/s2.log 2>&1 & S2=$!; sleep 2
H=$(ulimit -Hu); prlimit --pid $D --nproc=1:$H; prlimit --pid $S2 --nproc=1:$H
$P --servidor "unix:$T/d.sock" >/dev/null 2>&1 || true; $P --servidor 127.0.0.1:18498 >/dev/null 2>&1 || true
prlimit --pid $D --nproc=$H:$H; prlimit --pid $S2 --nproc=$H:$H
v=$($P --servidor "unix:$T/d.sock;127.0.0.1:18498" | tail -1); w=$($P --servidor 127.0.0.1:18498 | tail -1)
[ "$v" = "RESULTADO: OK" ] && [ "$w" = "RESULTADO: OK" ] && kill -0 $D $S2 && echo "RESULTADO: OK" || echo "RESULTADO: FALLO (sin hilos: agente o servidor caído)"
kill $D $S2; wait $D $S2 2>/dev/null || true
# a bad numeric option is a usage error (exit 2), not an abort
r=0; $B/service-ple-server --fichero $T/t.safetensors --puerto abc 2>/dev/null || r=$?; a=0; $B/service-ple-agent --socket $T/x.sock --filas abc 2>/dev/null || a=$?
[ $r = 2 ] && [ $a = 2 ] && echo "RESULTADO: OK" || echo "RESULTADO: FALLO (opciones: servidor $r, agente $a)"
