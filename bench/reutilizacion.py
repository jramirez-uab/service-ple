"""Medidas 1 y 2 de service-ple: reutilización de filas de la PLE en el tiempo y duplicados dentro de un prompt.

Usa el cálculo de ids de perfil.py (idéntico a vLLM). Por fuente, los primeros TOKENS tokens en orden de documento (y una MEZCLA
que intercala documentos de todas las fuentes, como una máquina con usuarios distintos).

Medida 1: para cada acceso, r = accesos desde el anterior a la misma fila (∞ si es la primera vez). Caché LRU de C filas con la aproximación
de Che: W(T) = E[min(r, T)] es el número medio de filas distintas en una ventana de T accesos; T_C cumple W(T_C) = C; acierto ≈ P(r ≤ T_C).
Medida 2: por documento usado como prompt (recortado a 2.048 y a 10.240 tokens; solo los que llegan a esa longitud), fracción de filas
repetidas dentro del propio prompt = lo que se ahorra pidiendo ids sin repetir.

uso: reutilizacion.py DIR_SALIDA   (needs numpy, pyarrow and tokenizers; PLE_DATASETS and PLE_TOKENIZER as in perfil.py)
"""
import os, sys, time
import numpy as np

OUT = sys.argv[1]
os.makedirs(OUT, exist_ok=True)
sys.argv = [sys.argv[0], os.path.join(os.path.dirname(os.path.abspath(__file__)), "salida")]  # perfil.py lee su DIR_SALIDA al importarse
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import perfil as P  # noqa: E402

TOKENS = int(os.environ.get("TOKENS", 4_000_000))
CACHES = [1 << 20, 4 << 20, 16 << 20, 64 << 20]
LARGOS = [2048, 10240]


def secuencia(nombre):
    """Guarda ids (uint32, tokens×16) y longitudes de documento de una fuente."""
    f_ids, f_doc = f"{OUT}/{nombre}.ids.npy", f"{OUT}/{nombre}.docs.npy"
    if os.path.exists(f_doc):
        return nombre
    tok = P.Tokenizer.from_file(P.TOK)
    tipo, ruta, campos = P.FUENTES[nombre]
    partes, largos, n, t0 = [], [], 0, time.time()
    for texto in P.textos(tipo, ruta, campos):
        t = np.array(tok.encode(texto).ids, np.int64)
        if len(t) == 0:
            continue
        partes.append(P.ids_de(t).astype(np.uint32).reshape(-1))
        largos.append(len(t))
        n += len(t)
        if n >= TOKENS:
            break
    np.save(f_ids, np.concatenate(partes))
    np.save(f_doc, np.array(largos, np.int64))
    print(f"{nombre}: {n:,} tokens, {len(largos):,} documentos ({time.time() - t0:.0f} s)", flush=True)
    return nombre


def reutilizacion(s):
    """r de cada acceso (int64; L+1 = nunca antes) y número de filas distintas."""
    L = len(s)
    orden = np.argsort(s, kind="stable")
    ss = s[orden]
    r = np.full(L, L + 1, np.int64)
    mismo = ss[1:] == ss[:-1]
    r[orden[1:][mismo]] = orden[1:][mismo] - orden[:-1][mismo]
    return r, int(L - mismo.sum())


def lru(r, L):
    """Acierto de LRU para cada tamaño de CACHES (aproximación de Che)."""
    rs = np.sort(r)
    acum = np.concatenate([[0], np.cumsum(rs, dtype=np.float64)])
    def W(T):  # E[min(r, T)]
        k = np.searchsorted(rs, T, side="right")
        return (acum[k] + T * (L - k)) / L
    out = []
    for C in CACHES:
        lo, hi = 1, L + 1
        if W(hi) <= C:
            out.append(np.searchsorted(rs, L, side="right") / L)
            continue
        while lo < hi:  # menor T con W(T) >= C
            m = (lo + hi) // 2
            if W(m) >= C: hi = m
            else: lo = m + 1
        out.append(np.searchsorted(rs, lo, side="right") / L)
    return out


def duplicados(s, largos):
    out = []
    for N in LARGOS:
        tot = uni = docs = 0
        ini = 0
        for l in largos:
            if l >= N:
                p = s[ini * 16:(ini + N) * 16]
                tot += len(p); uni += len(np.unique(p)); docs += 1
            ini += l
        out.append((docs, 1 - uni / tot if tot else float("nan")))
    return out


def mezcla(nombres):
    """Intercala documentos de todas las fuentes (uno de cada, por turno)."""
    datos = [(np.load(f"{OUT}/{n}.ids.npy"), np.load(f"{OUT}/{n}.docs.npy")) for n in nombres]
    pos = [np.concatenate([[0], np.cumsum(d)]) * 16 for _, d in datos]
    partes, largos = [], []
    for k in range(max(len(d) for _, d in datos)):
        for (s, d), p in zip(datos, pos):
            if k < len(d):
                partes.append(s[p[k]:p[k + 1]]); largos.append(d[k])
    return np.concatenate(partes), np.array(largos)


def informe_fuente(nombre):
    return informe(nombre, np.load(f"{OUT}/{nombre}.ids.npy"), np.load(f"{OUT}/{nombre}.docs.npy"))


def informe(nombre, s, largos):
    t0 = time.time()
    L = len(s)
    r, distintas = reutilizacion(s)
    acierto = lru(r, L)
    dup = duplicados(s, largos)
    fila = (f"| {nombre} | {L // 16:,} | {distintas:,} | {1 - distintas / L:.1%} | " + " | ".join(f"{a:.1%}" for a in acierto)
            + " | " + " | ".join(f"{p:.1%} ({d:,})" for d, p in dup) + " |")
    print(f"{nombre}: analizado en {time.time() - t0:.0f} s", flush=True)
    return fila


if __name__ == "__main__":
    import multiprocessing as mp
    nombres = list(P.FUENTES)
    with mp.get_context("fork").Pool(len(nombres)) as pool:
        pool.map(secuencia, nombres)
    with mp.get_context("fork").Pool(len(nombres)) as pool:
        filas = pool.map(informe_fuente, nombres)
    s, largos = mezcla(nombres)
    filas.append(informe("MEZCLA", s, largos))
    cab = ("| Fuente | Tokens | Filas distintas | Acierto con caché infinita | " + " | ".join(f"LRU {c >> 20} M ({c * 160 / 1e9:.1f} GB)" for c in CACHES)
           + " | " + " | ".join(f"Duplicados en prompt de {n:,} (prompts)" for n in LARGOS) + " |")
    texto = ("# Reutilización de filas de la PLE (medidas 1 y 2)\n\n"
             f"Primeros {TOKENS:,} tokens de cada fuente en orden de documento; MEZCLA intercala documentos de todas. LRU con la aproximación de Che "
             "(acierto en régimen, con la caché ya llena). Duplicados: filas repetidas dentro del mismo prompt (lo que no hace falta pedir).\n\n"
             + cab + "\n|" + "---|" * (cab.count("|") - 1) + "\n" + "\n".join(filas) + "\n")
    open(f"{OUT}/INFORME.md", "w").write(texto)
    print(texto, flush=True)
    print("FIN", flush=True)
