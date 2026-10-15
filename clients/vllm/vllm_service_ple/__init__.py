"""vllm-service-ple: the Qwen3.8 / Swift Flash-Next PLE table served by service-ple instead of being pinned in host RAM.

A vLLM general plugin (entry point `vllm.general_plugins`). Inert unless the server is started with
`--additional-config '{"service_ple": "unix:/run/service-ple/agente.sock;HOST:18460"}'` (local agent first, then the server).
The network code is the C++ library libserviceple.so (next to this file, or SERVICE_PLE_LIB).
"""


def register():
    from .service_ple import patch
    patch()
