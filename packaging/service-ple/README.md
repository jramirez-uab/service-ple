# service-ple

**Experimental, early stage.** The server and the agent of [service-ple](https://github.com/jramirez-uab/service-ple) as ready-built
Linux commands (C++, x86-64, glibc ≥ 2.24): they keep the PLE / n-gram embedding table of Qwen3.8 / Swift **Flash-Next** (51 GB) out of
the inference engines.

- `service-ple-agent`: one per machine, a cache shared by every engine on it; the rows it does not have come from the local table file
  (XFS on NVMe; not ZFS), a central server, or both.
- `service-ple-server`: the table in RAM, served over TCP to many machines (no GPU needed).
- `service-ple-install`: installs either one as a systemd service (idempotent; `-n` is a dry run).
- `service-ple-lecturas`: random 4 KB reads per second of a file system (is it fast enough for the agent?).

```
uv tool install service-ple                                                      # or: pipx install service-ple
sudo "$(command -v service-ple-install)" agent --table /xfs/ple-fp8.safetensors                 # local file
sudo "$(command -v service-ple-install)" agent --server HOST:18460 --table /xfs/ple-fp8.safetensors   # central server, file as fallback
sudo "$(command -v service-ple-install)" server --table /path/ple-fp8.safetensors --allow "ENGINE_HOST_1 ENGINE_HOST_2"
```

The vLLM side is the `vllm-service-ple` package (`pip install vllm-service-ple` in the vLLM 0.31 environment). Design, measurements and
source: https://github.com/jramirez-uab/service-ple
