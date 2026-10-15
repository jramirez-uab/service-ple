"""Análisis del perfil de acceso a la PLE: concentración por fuente y sesgo cruzado. uso: analiza.py DIR_SALIDA"""
import glob, json, os, sys
import numpy as np

D = sys.argv[1]
K = [1 << 20, 4 << 20, 16 << 20, 64 << 20]            # filas en caché; ×160 B = 0,16 / 0,64 / 2,6 / 10,7 GB
nombres = sorted(os.path.basename(f)[:-len(".cuenta.npy")] for f in glob.glob(f"{D}/*.cuenta.npy"))
C = {n: np.load(f"{D}/{n}.cuenta.npy", mmap_mode="r") for n in nombres}
tot = {n: float(C[n].sum(dtype=np.uint64)) for n in nombres}
top = {}
lin = ["# Perfil de acceso a la tabla PLE", "", f"Fuentes: {', '.join(nombres)}. Consultas = tokens × 16 filas.", ""]
lin += ["## Concentración (la caché se llena y se prueba con la misma fuente: optimista)", "",
        "| Fuente | Consultas | Filas distintas | " + " | ".join(f"top {k >> 20} M ({k * 160 / 1e9:.1f} GB)" for k in K) + " |",
        "|---|---|---|" + "---|" * len(K)]
for n in nombres:
    c = np.asarray(C[n])
    orden = np.argsort(c)[::-1]
    top[n] = orden[: K[-1]]
    cs = np.cumsum(c[orden[: K[-1]]], dtype=np.uint64)
    distintas = int(np.count_nonzero(c))
    lin.append(f"| {n} | {tot[n]:,.0f} | {distintas:,} | " + " | ".join(f"{100 * cs[k - 1] / tot[n]:.1f} %" for k in K) + " |")
    print(lin[-1], flush=True)
mezcla = sum(np.asarray(C[n], dtype=np.float64) / tot[n] for n in nombres)   # cada fuente pesa lo mismo
top["MEZCLA"] = np.argsort(mezcla)[::-1][: K[-1]]
for kk in (K[1], K[2]):
    lin += ["", f"## Sesgo cruzado: caché de {kk >> 20} M filas ({kk * 160 / 1e9:.1f} GB) llenada con la fila, probada con la columna", "",
            "| caché \\ prueba | " + " | ".join(nombres) + " |", "|---|" + "---|" * len(nombres)]
    for a in nombres + ["MEZCLA"]:
        h = top[a][:kk]
        lin.append(f"| {a} | " + " | ".join(f"{100 * float(np.asarray(C[b])[h].sum(dtype=np.uint64)) / tot[b]:.1f} %" for b in nombres) + " |")
        print(lin[-1], flush=True)
open(f"{D}/INFORME.md", "w").write("\n".join(lin) + "\n")
print("escrito", f"{D}/INFORME.md")
