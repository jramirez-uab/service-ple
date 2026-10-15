# Measured results (2026-10-15)

Setup: vLLM 0.31, Qwen3.8 / Swift Flash-Next EXL3 checkpoints, KV cache fp8 (L40S) or bf16 (RTX 3090, Ampere), service-ple server on a
separate machine (1 GbE link, ~0.5 ms round trip). "Local" = vLLM's default pinned host table.

## Table and access pattern
- `ple-fp8.safetensors`: 51.2 GB, 128 shards × 2,500,012 rows × 160 B (fp8 e4m3) + one bf16 scale; one PLE layer.
- 16 rows per token: bigram + trigram × 8 heads; row = hash(n-gram) mod per-head prime + offset (checked bit-exact against vLLM).
- vLLM pins it with PyTorch's caching host allocator, which rounds to a power of two: 64 GiB for TP 1, 2 × 32 GiB for TP 2 (split by vocabulary, not duplicated).

## Client and server
| | median | p99 |
|---|---|---|
| decode step, 1 session (16 rows) | 0.50 ms | 0.68 ms |
| decode step, 8 sessions (128 rows) | 0.68 ms | 0.80 ms |
| prefill chunk of 2,048 tokens (32,768 rows) | 56 ms (92 MB/s, the 1 GbE limit) | 76 ms |

Server load time: 33-41 s for 51.2 GB from ZFS (1.3-1.6 GB/s).

## End to end in vLLM
Flash-Next 2.25 bpw, TP 2 + EP, 2 × RTX 3090:

| | local table | service-ple |
|---|---|---|
| host RAM of the job (cgroup) | 77 GB | 7.3 GB |
| 1 session | 48.3 tok/s | 47.6 tok/s |
| 8 sessions | 151.7 tok/s | 150.6 tok/s |
| prefill of 10.8k tokens | 6.19 s | 6.29 s |

The engine is not deterministic run to run even with the local table (mean |Δ logprob| per token 0.072 between two local runs);
local vs service-ple: 0.073, the same noise. Rows are byte-identical (checked on 100k random rows).

2 × L40S, KV fp8, service-ple (host RAM of the job: 9-10 GB):
- 2.25 bpw: 52 / 269 tok/s (1 / 8 sessions); MTP 1: 84.5 / 347; MTP 3: 107 / 321. Prefill of 10.8k tokens: 3.1 s.
- 2.25 bpw, 262k context, 20 GB of KV per GPU (2.29 M tokens): needle retrieval 8/8 at 62k, 123k and 236k (prefill ~3,800 tok/s);
  8 sessions of ~138k tokens at once without preemption (12 tok/s each); a new 139k prompt stalls the 4 generating sessions at most 1.0 s.
- 3.75 bpw (model directory copied **without** the 51 GB table file): 89 / 150 / 237 / 369 tok/s with 1 / 2 / 4 / 8 requests; tool calls 29/30,
  reasoning 18/18, vision 15/15, sysadmin agent tasks 9/10 — the same quality as the other Flash-Next quantizations we tested.

## Missing rows (filled with zeros; always the same rows, chosen by a hash)
| missing | NLL | change |
|---|---|---|
| 0 % | 1.5532-1.5538 | — |
| 1 % | 1.5546 | +0.08 % |
| 5 % | 1.5552 | +0.12 % |
| 20 % | 1.5601 | +0.43 % |
| 50 % | 1.5751 | +1.4 % |
| 100 % (no PLE) | 1.5994 | +3.0 % |

Filling with the same head's row of the previous token instead of zeros: 1.5563 at 5 %, 1.5591 at 20 % — the same as zeros.

## Robustness
- Server stopped in the middle of a 2,500-token answer: the answer completes (zeros for the missing rows); when the server is back after 57 s,
  the engine reconnects on its own ("662 steps and 42,064 rows were zeros").
- Hung server (SIGSTOP): one step waits 2 s (`SERVICE_PLE_ESPERA_MS`), then zeros until it answers again.

## Agent cache (16 M rows, 2.8 GB)
- Two consecutive vLLM runs on the same texts: 26-32 % hits cold, **97 %** warm; speed unchanged (the network path is already fast).
- Access profile, 6 sources (English/Spanish/Catalan/Chinese Wikipedia, arXiv abstracts, code), 4 M tokens each + a 24 M-token mix,
  steady-state LRU (Che approximation): mix 42 % (1 M rows) · 52 % (4 M) · 62 % (16 M) · 72 % (64 M); per source at 16 M: 46 % (zh) to 92 % (code).
  Strong cross-domain bias: a 4 M cache filled from Spanish Wikipedia hits 66 % there and 10-26 % elsewhere → the cache must learn online.
- 17-19 % of a 2k-token prompt's rows and 22-30 % of a 10k one are duplicates (the client deduplicates before asking).

Raw reports: `perfil-INFORME.md`, `reutilizacion-INFORME.md`.
