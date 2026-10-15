"""service-ple (phase 1): the Flash-Next PLE table is served over TCP instead of being pinned in host RAM.

Enabled per server with `--additional-config '{"service_ple": "host:port"}'`. When the key is absent nothing changes.
When present:
- `ple-fp8.safetensors` is dropped from the files vLLM loads (no 51 GB read, no 64 GiB pinned);
- the table parameter is a 1-row pinned tensor expanded to the full shape (shape checks still hold);
- `_lookup` queues three steps on the PLE prefetch stream, all CUDA-graph capturable: n-gram ids GPU→pinned host, a host node
  (`cuLaunchHostFunc`, in the C++ library) that asks the server for the rows of this rank's vocabulary range (zeros elsewhere, so
  the TP all-reduce of raw FP8 bytes stays exact), and rows pinned host→GPU. The network wait overlaps the previous layer;
  Python never blocks. Errors inside the host node write zeros and are raised on the next lookup;
- the global scale comes from the server, which is checked against the local safetensors header when the file exists.
The network code is the C++ library `libserviceple.so` (C ABI, loaded with ctypes): `SERVICE_PLE_LIB` or next to this file.
"""
import ctypes
import json
import os
import struct

_ESTADO = {"direccion": None}


def _direccion():
    if _ESTADO["direccion"] is None:
        try:
            from vllm.config import get_current_vllm_config
            extra = get_current_vllm_config().additional_config or {}
            _ESTADO["direccion"] = extra.get("service_ple", "") if isinstance(extra, dict) else ""
        except Exception:
            return ""
    return _ESTADO["direccion"]


def _lib():
    if "lib" not in _ESTADO:
        ruta = os.environ.get("SERVICE_PLE_LIB") or os.path.join(os.path.dirname(__file__), "libserviceple.so")
        lib = ctypes.CDLL(ruta)
        lib.sple_abrir.restype = ctypes.c_void_p
        lib.sple_abrir.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
        lib.sple_info.argtypes = [ctypes.c_void_p] + [ctypes.c_void_p] * 4
        lib.sple_filas.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int64, ctypes.c_int64,
                                   ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
        lib.sple_encolar.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint64, ctypes.c_int64, ctypes.c_int64,
                                     ctypes.c_void_p, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
        lib.sple_errores.restype = ctypes.c_uint64
        lib.sple_errores.argtypes = [ctypes.c_char_p, ctypes.c_int]
        _ESTADO["lib"] = lib
    return _ESTADO["lib"]


def _conexion():
    if "h" not in _ESTADO:
        lib, err = _lib(), ctypes.create_string_buffer(512)
        h = lib.sple_abrir(_direccion().encode(), err, len(err))
        if not h:
            raise RuntimeError(f"service-ple: {err.value.decode(errors='replace')}")
        filas, bf, esc, huella = ctypes.c_uint64(), ctypes.c_uint32(), ctypes.c_uint16(), ctypes.c_uint64()
        lib.sple_info(h, ctypes.byref(filas), ctypes.byref(bf), ctypes.byref(esc), ctypes.byref(huella))
        _ESTADO.update(h=h, filas=filas.value, bytes_fila=bf.value, escala_bf16=esc.value, huella=huella.value)
    return _ESTADO["h"]


def _huella_local(ruta):
    """FNV-1a 64 of the safetensors JSON header (same as the server) and the bf16 scale bytes."""
    with open(ruta, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        cab = f.read(n)
        h = 0xCBF29CE484222325
        for b in cab:
            h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
        meta = json.loads(cab)
        clave = next(k for k in meta if k.endswith("weight_scale"))
        f.seek(8 + n + meta[clave]["data_offsets"][0])
        return h, struct.unpack("<H", f.read(2))[0]


def patch():
    import torch
    from vllm.logger import init_logger
    try:
        from vllm.models.qwen4_exp.common import ngram_embedding as ne
        from vllm.model_executor.model_loader import default_loader as dl
    except Exception:
        return
    log = init_logger(__name__)
    cls = ne.Qwen4ExpPLEPinnedHostEmbedding
    if getattr(cls, "_service_ple_parcheado", False):
        return

    orig_alloc = cls.allocate_embedding_weight

    def allocate_embedding_weight(self, num_embeddings, embedding_dim, dtype):
        if not _direccion():
            return orig_alloc(self, num_embeddings, embedding_dim, dtype)
        self._sple_remoto = True
        _conexion()
        if _ESTADO["bytes_fila"] != embedding_dim * torch.empty((), dtype=dtype).element_size():
            raise RuntimeError(f"service-ple: el servidor da filas de {_ESTADO['bytes_fila']} B y el modelo espera {embedding_dim} × {dtype}")
        log.info("service-ple: tabla PLE remota en %s (%d filas × %d B, huella %016x); sin memoria fijada para la tabla",
                 _direccion(), _ESTADO["filas"], _ESTADO["bytes_fila"], _ESTADO["huella"])
        return torch.empty((1, embedding_dim), dtype=dtype, device="cpu", pin_memory=True).expand(num_embeddings, embedding_dim)

    orig_lookup = cls._lookup

    def _lookup(self, input_ids, output=None):
        if not getattr(self, "_sple_remoto", False):
            return orig_lookup(self, input_ids, output)
        dim = self.embedding_dim
        if output is None:
            output = torch.empty((*input_ids.shape, dim), dtype=self.weight.dtype, device=input_ids.device)
        n = input_ids.numel()
        if n == 0:
            return output
        lib, err = _lib(), ctypes.create_string_buffer(512)
        if lib.sple_errores(err, len(err)) != _ESTADO.get("errores_vistos", 0):
            raise RuntimeError(f"service-ple: fallo en una petición encolada: {err.value.decode(errors='replace')}")
        st = getattr(self, "_sple_buf", None)
        if st is None or st[0].numel() < n:
            # fixed addresses: a captured CUDA graph keeps them, so allocate the maximum once (tokens × heads of the prefetch buffer)
            if torch.cuda.is_current_stream_capturing():
                raise RuntimeError("service-ple: buffers too small during CUDA graph capture")
            cap = max(n, self._prefetch_buffer.shape[0] * self._prefetch_buffer.shape[1])
            st = (torch.empty(cap, dtype=torch.int64, pin_memory=True), torch.empty(cap * dim, dtype=torch.uint8, pin_memory=True))
            self._sple_buf = st
        ids_h, filas_h = st
        # three steps on the current (prefetch) stream, all capturable: ids GPU→host, network fetch as a host node, rows host→GPU
        ids_h[:n].copy_(input_ids.reshape(-1), non_blocking=True)
        si = self.shard_indices
        if lib.sple_encolar(_conexion(), ctypes.c_void_p(torch.cuda.current_stream().cuda_stream), ids_h.data_ptr(), n,
                            si.org_vocab_start_index, si.org_vocab_end_index, filas_h.data_ptr(),
                            int(torch.cuda.is_current_stream_capturing()), err, len(err)):
            raise RuntimeError(f"service-ple: {err.value.decode(errors='replace')}")
        output.view(torch.uint8).reshape(-1)[: n * dim].copy_(filas_h[: n * dim], non_blocking=True)
        return output

    # the global FP8 scale lives in the dropped file: take it from the server (checked against the local header if present)
    fp8 = ne.Qwen4ExpPLEFp8EmbeddingMethod
    orig_pwal = fp8.process_weights_after_loading

    def process_weights_after_loading(self, layer):
        if getattr(layer, "_sple_remoto", False):
            esc = torch.tensor([_ESTADO["escala_bf16"]], dtype=torch.int16).view(torch.bfloat16).float()
            layer.weight_scale.data.copy_(esc.to(layer.weight_scale.device).reshape(layer.weight_scale.shape))
            ruta = _ESTADO.get("fichero_local")
            if ruta and os.path.exists(ruta):
                h, e = _huella_local(ruta)
                if (h, e) != (_ESTADO["huella"], _ESTADO["escala_bf16"]):
                    raise RuntimeError(f"service-ple: el servidor sirve otra tabla (huella {_ESTADO['huella']:016x}, la del modelo {h:016x})")
                log.info("service-ple: el servidor sirve la misma tabla que %s (huella y escala comprobadas)", ruta)
        return orig_pwal(self, layer)

    orig_prep = dl.DefaultModelLoader._prepare_weights

    def _prepare_weights(self, *a, **k):
        hf_folder, files, use_st, index = orig_prep(self, *a, **k)
        if _direccion():
            fuera = [f for f in files if os.path.basename(f) == "ple-fp8.safetensors"]
            if fuera:
                _ESTADO["fichero_local"] = fuera[0]
                files = [f for f in files if f not in fuera]
                log.info("service-ple: no se carga %s (la tabla la da el servidor)", fuera[0])
        return hf_folder, files, use_st, index

    # a model directory may come without ple-fp8.safetensors at all (copied without the 51 GB table): vLLM checks that every file in
    # the index exists, so in remote mode a missing table file is not an error (the server's table is then not checked against it)
    orig_fdsf = dl.filter_duplicate_safetensors_files

    def filter_duplicate_safetensors_files(files, folder, index_file):
        try:
            return orig_fdsf(files, folder, index_file)
        except FileNotFoundError:
            tabla = os.path.join(folder, "ple-fp8.safetensors")
            if not _direccion() or os.path.exists(tabla):
                raise
            log.info("service-ple: %s no está en el directorio del modelo; la tabla del servidor no se puede comparar con la local", tabla)
            return [f for f in orig_fdsf(files + [tabla], folder, index_file) if f != tabla]

    dl.filter_duplicate_safetensors_files = filter_duplicate_safetensors_files
    cls.allocate_embedding_weight = allocate_embedding_weight
    cls._lookup = _lookup
    fp8.process_weights_after_loading = process_weights_after_loading
    dl.DefaultModelLoader._prepare_weights = _prepare_weights
    cls._service_ple_parcheado = True
