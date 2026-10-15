# service-ple

Serve the **PLE / n-gram embedding table** of Qwen3.8 / Swift **Flash-Next** (and Engram-style models) from a network service, instead of
pinning it in the host RAM of every inference server.

The Flash-Next table is 51.2 GB (fp8, 320 M rows × 160 B). vLLM keeps it in pinned host memory, rounded up to a power of two by PyTorch's
pinned allocator: **64 GiB of RAM per server instance**, on every machine that runs one. The rows a token needs depend only on the token ids
(16 rows per token: bigram + trigram × 8 heads), so they can come from anywhere, as long as they arrive in time.

```
  engine ──(thin plugin)──→ libserviceple.so ──→ service-ple-agent (per machine) ──→ service-ple-server (table in RAM)
                                 │                   ├─ shared cache in /dev/shm         (one per site / cluster)
                                 └─ reads the shared └─ local file fallback (after N network failures, or alone without a server)
                                    cache directly
```

| Component | What it does |
|---|---|
| `service-ple-server` | Loads the table once into RAM (1 GiB pages when available) and returns rows over TCP. No GPU, no fp8 hardware: it only copies bytes. An old 64 GB machine is enough. |
| `service-ple-agent` | One per machine, shared by every engine on it. Keeps a cache of hot rows in shared memory (set-associative, CLOCK eviction, seqlock per slot) that engines read without locks; asks the server for misses; falls back to the local table file after N consecutive network failures, or works disk-only without a server. |
| `libserviceple.so` | Client library with a C ABI. Requests unique sorted ids, scatters rows, fills zeros outside the rank's vocabulary range (exact under tensor parallelism). Degrades to zeros if nothing answers, reconnects on its own, warns in stderr. `sple_encolar` enqueues the fetch as a CUDA **host node** (`cuLaunchHostFunc`), so it works inside captured CUDA graphs. |
| `clients/vllm` | vLLM general plugin (`vllm-service-ple`). Enabled per server with `--additional-config '{"service_ple": "unix:/run/service-ple/agente.sock;HOST:18460"}'`; inert otherwise. Skips loading the table file, allocates a 1-row placeholder, checks the server's table fingerprint and scale. |

## Measured (2026-10-15, vLLM 0.31, Flash-Next EXL3 2.25 bpw, TP 2 + EP)

| | local pinned table | service-ple (1 GbE) |
|---|---|---|
| Host RAM of the job (2 × RTX 3090) | 77 GB | **7.3 GB** |
| 1 / 8 sessions | 48.3 / 151.7 tok/s | 47.6 / 150.6 tok/s (−1.4 % / −0.7 %) |
| Prefill, 10.8k tokens | 6.19 s | 6.29 s |
| Log-likelihood | identical within run-to-run noise (rows are byte-identical) | |

- 2 × L40S, KV fp8, MTP 3: 107 tok/s with one session; 8 sessions of ~138k tokens at once without preemption; 8/8 needles at 236k; 10 GB of host RAM.
- Agent with a 16 M-row cache (2.8 GB): 97 % hits on repeated content; 26-32 % on new text.
- Access profile (6 sources, 24 M tokens): an LRU cache of 16 M rows gets ~62 % hits on a mix; strong cross-domain bias (a cache learned on
  Spanish Wikipedia gets 10-26 % on other sources), so the cache must learn online. 17-30 % of a prompt's rows are duplicates.
- Missing rows filled with zeros: +0.1 % NLL at 5 % missing, +0.4 % at 20 %, +3.0 % without the table at all. Zeros are fine for short outages.
- Server killed in the middle of a 2,500-token answer: the answer completes; the engine reconnects when the server is back.

Details and raw numbers: `docs/RESULTS.md` and the access-profile reports in `docs/`.

## Prior and related work (what is new here and what is not)

- The idea of a **hierarchical embedding parameter server** (GPU cache → CPU → SSD / remote) is well established in recommender systems:
  NVIDIA Merlin HugeCTR's Hierarchical Parameter Server (arXiv 2210.08804), TorchRec, Monolith. service-ple applies it to LLM inference.
- **DeepSeek Engram** (arXiv 2601.07372) keeps a 100B-parameter table in host DRAM and prefetches rows (≤2.8 % overhead). **TF-Engram**
  (arXiv 2607.07388) adds an SSD-DRAM-GPU hierarchy. Gemma 3n caches its PLE on fast local storage.
- In engines: vLLM pins the table in host memory (default) and has a draft for mmap-from-disk offload (#54070) and a request for
  auxiliary-GPU offload (#53908); SGLang has an open PR streaming PLE from NVMe with io_uring (#36567). All of these are **per machine**.
- New in service-ple, as far as we found: one copy of the table **shared over the network** by many engines and machines; a per-machine
  shared cache that learns online; graceful degradation measured on the model (zeros, disk fallback); CUDA-graph-compatible remote fetch;
  works with tensor/expert parallelism; engine-independent protocol.

## Deploy (idempotent)

```
deploy/install.sh server --table /path/ple-fp8.safetensors --allow "ENGINE_HOST_1 ENGINE_HOST_2"   # machine with the table in RAM
deploy/install.sh agent  --server SERVER_HOST:18460 --table /local/copy/ple-fp8.safetensors      # each engine machine (table optional)
deploy/install.sh plugin --python /path/to/vllm-venv/bin/python                                  # each vLLM environment
```
Each run compares what is installed with what it would install, replaces only what differs (atomically), restarts only what uses a
changed file, and prints what changed; a second run prints `no changes`. `-n` is a dry run, `--no-restart` leaves restarts pending.

## Build and test

```
cmake -S . -B build && cmake --build build -j
cmake -S . -B build-asan -DSPLE_SANITIZE=ON && cmake --build build-asan -j && tests/run.sh build-asan
```
C++20 (GCC ≥ 11), Linux, no dependencies beyond the standard library (`libcuda.so.1` is loaded at run time, only for `sple_encolar`).
Built and tested (`tests/run.sh`, no warnings) on Ubuntu 22.04 (GCC 11.4), Ubuntu 24.04 (GCC 13.3), Debian 12 (GCC 12.2), Rocky Linux 9
(GCC 11.5) and Ubuntu 26.04 (GCC 15.2), x86-64. The protocol assumes little-endian hosts (x86-64, ARM64); ARM64 is untested.
`deploy/install.sh` needs systemd (and nftables for `--allow`).

## Status

**Experimental.** Tested with Qwen3.8 / Swift Flash-Next in vLLM 0.31 (EXL3 checkpoints, TP 1/2 + EP, RTX 3090, L40S, RTX PRO 6000).
Code comments and the measurement reports in `docs/` are mostly in Spanish.

## License

Apache License 2.0 (`LICENSE`). Copyright 2026 Jorge Ramírez Espejo (`NOTICE`).

Developed with the support of the Department of Computer Science (Departament de Ciències de la Computació), Universitat Autònoma de
Barcelona, which provided the hardware. Claude (Anthropic) co-authored the code, as the commit trailers show.
