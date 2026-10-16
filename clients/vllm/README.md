# vllm-service-ple

**Experimental, early stage.** vLLM general plugin that keeps the PLE / n-gram embedding table of Qwen3.8 / Swift **Flash-Next** out of
the vLLM process. vLLM normally pins the 51 GB table in host RAM (64 GiB per server); with this plugin it allocates a 1-row placeholder
and fetches the rows of each step from [service-ple](https://github.com/jramirez-uab/service-ple):

- a **per-machine agent** with a cache shared by every engine on the machine, whose misses come from the local table file, a central
  server, or both;
- or a **central server** directly.

Independent of the checkpoint format (EXL3, FP8, NVFP4): it hooks `Qwen4ExpPLEPinnedHostEmbedding` in **vLLM 0.31**. It patches vLLM
internals (`_lookup`, the weight-file list, the FP8 scale check), so it is tied to that vLLM version.

```
pip install vllm-service-ple                       # in the vLLM environment; includes the client library (libserviceple.so)
vllm serve MODEL ... --additional-config '{"service_ple": "unix:/run/service-ple/agente.sock"}'   # local agent
vllm serve MODEL ... --additional-config '{"service_ple": "SERVER_HOST:18460"}'                    # central server
```

The agent and the server are system services, not part of this package: build and install them from the repository
(`deploy/install.sh agent ...`, `deploy/install.sh server ...`). Inert when `service_ple` is not set.
