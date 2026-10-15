"""Perfil de acceso a la tabla PLE de Flash-Next: qué filas se leen con textos de distinto tipo.

Reproduce el cálculo de ids de vLLM 0.31 (vllm/models/qwen4_exp/nvidia/ngram_embedding.py, compute_ngram_ids): para cada token, bigrama y trigrama
× 8 cabezas = 16 filas; hash = token·m0 ^ prev1·m1 [^ prev2·m2] (int64 con desbordamiento), módulo el primo de cada cabeza, + desplazamiento.
Antes del principio de cada documento se usa el EOS, como hace vLLM con las fronteras.

Se puede parar y reanudar: cada fuente se procesa en trozos; al acabar un trozo se guarda el recuento acumulado (escritura atómica) y el
estado. SIGTERM (systemctl stop, apagado) termina el trozo en curso, lo guarda y sale con 0; al rearrancar sigue donde estaba.

uso: perfil.py DIR_SALIDA            (fuentes fijas abajo)
"""
import gzip, json, os, signal, sys, time
import numpy as np
import pyarrow.parquet as pq
from tokenizers import Tokenizer

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
DS = os.environ.get("PLE_DATASETS", "/path/to/datasets")   # parquet / jsonl.gz sources listed in FUENTES
TOK = os.environ.get("PLE_TOKENIZER", "/path/to/Flash-Next/tokenizer.json")
TOKENS_POR_FUENTE = 20_000_000
TROZO = 4_000_000                       # tokens por punto de control

FUENTES = {   # nombre -> (tipo, ruta, campo(s))
    "wiki-en": ("parquet", f"{DS}/ple-perfil/wikimedia__wikipedia/20231101.en/train-00000-of-00041.parquet", ["text"]),
    "wiki-es": ("parquet", f"{DS}/ple-perfil/wikimedia__wikipedia/20231101.es/train-00000-of-00013.parquet", ["text"]),
    "wiki-ca": ("parquet", f"{DS}/ple-perfil/wikimedia__wikipedia/20231101.ca/train-00000-of-00004.parquet", ["text"]),
    "wiki-zh": ("parquet", f"{DS}/ple-perfil/wikimedia__wikipedia/20231101.zh/train-00000-of-00006.parquet", ["text"]),
    "arxiv": ("jsonl.gz", f"{DS}/ple-perfil/gfissore__arxiv-abstracts-2021/arxiv-abstracts.jsonl.gz", ["title", "abstract"]),
    "codigo": ("parquet", f"{DS}/nvidia/OpenCodeInstruct/data/train-00004-of-00050.parquet", ["input", "output"]),
}

# --- parámetros del hash (config de Flash-Next + código de vLLM) ---
MASK = (1 << 64) - 1
def splitmix64(v):
    v = (v + 0x9E3779B97F4A7C15) & MASK
    v = ((v ^ (v >> 30)) * 0xBF58476D1CE4E5B9) & MASK
    v = ((v ^ (v >> 27)) * 0x94D049BB133111EB) & MASK
    return (v ^ (v >> 31)) & MASK
def is_prime(n):
    if n < 2: return False
    for p in (2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31, 37):
        if n % p == 0: return n == p
    d, s = n - 1, 0
    while d % 2 == 0: d //= 2; s += 1
    for a in (2, 325, 9375, 28178, 450775, 9780504, 1795265022):
        if a % n == 0: continue
        x = pow(a, d, n)
        if x in (1, n - 1): continue
        for _ in range(s - 1):
            x = pow(x, 2, n)
            if x == n - 1: break
        else: return False
    return True
VOCAB, EOS, NGRAM, HEADS, BASE, SEED, LAYER = 248320, 248044, 3, 8, 20_000_000, 1234, 0
half = max(1, (((1 << 63) - 1) // VOCAB) // 2)
MULT = [2 * (splitmix64(SEED + 10007 * LAYER + 0x9E3779B97F4A7C15 * (i + 1)) % half) + 1 for i in range(NGRAM)]
SIZES, OFFS, off, p = [], [], 0, BASE - 1
for h in range((NGRAM - 1) * HEADS):
    c = p + 1
    if c % 2 == 0: c += 1
    while not is_prime(c): c += 2
    p = c; SIZES.append(c); OFFS.append(off); off += c
NROWS = off
SIZES_A, OFFS_A = np.array(SIZES, np.int64), np.array(OFFS, np.int64)
M = np.array(MULT, dtype=np.uint64).view(np.int64)          # multiplicadores como int64 (mismo patrón de bits que torch.long)

def ids_de(doc):
    """doc: array int64 de tokens de UN documento -> (len, 16) ids de fila."""
    t = doc
    p1 = np.concatenate([[EOS], t[:-1]])
    p2 = np.concatenate([[EOS, EOS], t[:-2]])[: len(t)]
    with np.errstate(over="ignore"):
        m2 = (t * M[0]) ^ (p1 * M[1])
        m3 = m2 ^ (p2 * M[2])
    out = np.empty((len(t), 16), np.int64)
    out[:, :8] = np.remainder(m2[:, None], SIZES_A[:8]) + OFFS_A[:8]
    out[:, 8:] = np.remainder(m3[:, None], SIZES_A[8:]) + OFFS_A[8:]
    return out

def textos(tipo, ruta, campos):
    if tipo == "parquet":
        f = pq.ParquetFile(ruta)
        for b in f.iter_batches(batch_size=512, columns=campos):
            cols = [b.column(c).to_pylist() for c in campos]
            for fila in zip(*cols):
                yield "\n".join(x for x in fila if x)
    else:
        with gzip.open(ruta, "rt") as fh:
            for linea in fh:
                d = json.loads(linea)
                yield "\n".join(d.get(c) or "" for c in campos)

parar = False
def _sig(*_):
    global parar
    parar = True
    print("SIGTERM: termino el trozo en curso y salgo", flush=True)
signal.signal(signal.SIGTERM, _sig)

def guarda_atomico(ruta, fn):
    tmp = ruta + ".tmp"
    fn(tmp)
    os.replace(tmp, ruta)

def procesa(nombre):
    tipo, ruta, campos = FUENTES[nombre]
    tok = Tokenizer.from_file(TOK)
    est_f = f"{OUT}/{nombre}.estado.json"
    est = json.load(open(est_f)) if os.path.exists(est_f) else {"docs": 0, "tokens": 0, "hecho": False}
    if est["hecho"]:
        print(f"{nombre}: ya hecho ({est['tokens']:,} tokens)", flush=True); return True
    cnt_f = f"{OUT}/{nombre}.cuenta.npy"
    cuenta = np.load(cnt_f) if os.path.exists(cnt_f) else np.zeros(NROWS, np.uint32)
    gen = textos(tipo, ruta, campos)
    for _ in range(est["docs"]):            # saltar lo ya contado (los textos se leen en orden determinista)
        next(gen)
    t0 = time.time()
    while est["tokens"] < TOKENS_POR_FUENTE and not parar:
        lote, n_tok, n_docs = [], 0, 0
        while n_tok < TROZO:
            try: txts = [next(gen) for _ in range(256)]
            except StopIteration: txts = []
            if not txts: break
            for e in tok.encode_batch(txts, add_special_tokens=False):
                a = np.array(e.ids, np.int64)
                if len(a): lote.append(ids_de(a).ravel()); n_tok += len(a)
            n_docs += len(txts)
        if not lote: break
        ids = np.concatenate(lote)
        cuenta += np.bincount(ids, minlength=NROWS).astype(np.uint32)
        est["docs"] += n_docs; est["tokens"] += n_tok
        guarda_atomico(cnt_f, lambda t: np.save(open(t, "wb"), cuenta))
        guarda_atomico(est_f, lambda t: json.dump(est, open(t, "w")))
        print(f"{nombre}: {est['tokens']:,} tokens, {est['docs']:,} docs ({time.time() - t0:.0f} s)", flush=True)
    if parar:
        print(f"{nombre}: parado limpiamente", flush=True); return False
    est["hecho"] = True
    guarda_atomico(est_f, lambda t: json.dump(est, open(t, "w")))
    print(f"{nombre}: HECHO {est['tokens']:,} tokens", flush=True)
    return True


if __name__ == "__main__":
    import multiprocessing as mp
    print(f"filas de la tabla {NROWS:,}; multiplicadores {MULT}; primos {SIZES[0]}..{SIZES[-1]}; {len(FUENTES)} fuentes en paralelo", flush=True)
    # una fuente por proceso (la conversión a tokens y numpy van en un núcleo cada uno); SIGTERM llega a todos los procesos del cgroup
    with mp.get_context("fork").Pool(len(FUENTES)) as pool:
        ok = pool.map(procesa, list(FUENTES))
    print("FIN" if all(ok) else "parado limpiamente", flush=True)
