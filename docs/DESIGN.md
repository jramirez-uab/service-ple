# Design

## Protocol (version 1, little-endian, TCP or Unix socket)
Request = 16-byte header `{magic "PLE1", version, op, n, status}` [+ n × uint32 row ids]. Response = header [+ body].
- `INFO` (op 1): `{rows, bytes_per_row, bf16 scale, fingerprint}`; the fingerprint is FNV-1a 64 of the safetensors JSON header, so a client can
  check that the server serves the same table as its local checkpoint.
- `ROWS` (op 2): n ids (sorted and unique, recommended) → n × bytes_per_row, same order. Status ≠ 0 means no body.
  `ERR_SIN_SERVIDOR` (6) from an agent means "no source for these rows right now": the connection stays open and the engine fills zeros.

**Idempotence.** Every operation is a read of an immutable table: repeating a request returns the same bytes and changes nothing on the
server. Clients may retry any request (after a reconnection, a timeout, or on another replica) without coordination. The agent's cache, the
firewall rules and the installer are idempotent too: starting the agent again reuses the same segment if the table and size match, the unit
reloads the nftables table from scratch, and `deploy/install.sh` only touches what changed.

## Server (`service-ple-server`)
Reads the safetensors header, places the shards by number (they are stored in alphabetical order), loads the whole table with parallel
`pread` into anonymous memory (1 GiB huge pages if reserved, otherwise THP), optionally `mlock`s it, and answers with one thread per connection.
It never converts fp8: the engine's GPU dequantizes, as it already does with the local table.

## Client library (`libserviceple.so`, C ABI)
- `sple_abrir("dest;dest;...")`: destinations in order of preference (`unix:/path` for the local agent, `host:port` for a server).
- `sple_filas(ids, n, lo, hi, out)`: only ids in `[lo, hi)` are requested (deduplicated); others get zeros. Under tensor parallelism each rank
  owns a vocabulary range, so the engine's all-reduce of raw fp8 bytes stays exact.
- `sple_encolar(stream, ...)`: the same, enqueued on a CUDA stream as a host node (`cuLaunchHostFunc`, driver API loaded at run time). vLLM
  captures the PLE lookup inside its CUDA graphs; the remote fetch becomes three capturable steps (ids GPU→pinned host, host node, rows
  pinned host→GPU) on the PLE prefetch stream, so the network wait overlaps the previous layer and Python never blocks.
- Failure handling: bounded waits (connect 300 ms, reply 2 s), immediate reconnect after a broken connection, then every 5 s; zeros meanwhile;
  warnings on stderr when it starts, every minute and when it recovers. `SERVICE_PLE_DEGRADAR=0` fails instead.
- Reads the agent's shared cache directly (no syscall on a hit) and asks only for the misses.

## Agent (`service-ple-agent`)
- Shared cache `/dev/shm/service-ple-<fingerprint>`: set-associative (8 ways), CLOCK eviction with a use bit set by readers, one seqlock
  version per slot (a reader that sees a slot being rewritten treats it as a miss). The table is read-only, so there is no coherence problem.
  One writer (the agent), many lock-free readers (the engines). The segment survives engine and agent restarts.
- Misses: from the server; after `--fallos-para-disco` consecutive network failures (10), from the local table file (`--fichero`, same
  fingerprint required) with parallel `pread`, retrying the network every `--reintento-red` seconds (30). Without `--servidor` it is disk-only.

## Phases
1. Server + client + engine hook (done). 2. Per-machine agent with shared cache (done). 3. Hide latency further (prefetch with MTP drafts,
batch prefill requests). 4. Robustness (zeros, reconnection, disk fallback: done; replicas: open). 5. Other engines (TabbyAPI/exllamav3, SGLang).
