# vllm-service-ple

vLLM general plugin: the Qwen3.8 / Swift Flash-Next PLE table is fetched from service-ple instead of being pinned in host RAM.
Independent of the checkpoint format (EXL3, FP8, NVFP4): it hooks `Qwen4ExpPLEPinnedHostEmbedding` in vLLM 0.31.

```
pip install -e .            # with libserviceple.so copied into vllm_service_ple/ (or SERVICE_PLE_LIB=/path/to/libserviceple.so)
vllm serve MODEL ... --additional-config '{"service_ple": "unix:/run/service-ple/agente.sock;SERVER_HOST:18460"}'
```
Inert when `service_ple` is not set. It patches vLLM internals (`_lookup`, the weight-file list, the FP8 scale check), so it is tied to the
vLLM version it was tested with.
