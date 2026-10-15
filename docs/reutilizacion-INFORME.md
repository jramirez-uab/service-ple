# Reutilización de filas de la PLE (medidas 1 y 2)

Primeros 4,000,000 tokens de cada fuente en orden de documento; MEZCLA intercala documentos de todas. LRU con la aproximación de Che (acierto en régimen, con la caché ya llena). Duplicados: filas repetidas dentro del mismo prompt (lo que no hace falta pedir).

| Fuente | Tokens | Filas distintas | Acierto con caché infinita | LRU 1 M (0.2 GB) | LRU 4 M (0.7 GB) | LRU 16 M (2.7 GB) | LRU 64 M (10.7 GB) | Duplicados en prompt de 2,048 (prompts) | Duplicados en prompt de 10,240 (prompts) |
|---|---|---|---|---|---|---|---|---|---|
| wiki-en | 4,007,504 | 26,499,804 | 58.7% | 40.1% | 48.8% | 57.3% | 58.7% | 18.3% (490) | 26.6% (121) |
| wiki-es | 4,000,038 | 22,434,118 | 64.9% | 42.6% | 54.4% | 64.2% | 64.9% | 16.7% (455) | 25.9% (104) |
| wiki-ca | 4,001,029 | 17,779,488 | 72.2% | 52.2% | 64.4% | 72.1% | 72.2% | 19.4% (464) | 30.3% (101) |
| wiki-zh | 4,000,398 | 32,987,125 | 48.5% | 30.9% | 37.6% | 46.0% | 48.5% | 16.9% (480) | 21.8% (107) |
| arxiv | 4,000,175 | 22,079,034 | 65.5% | 40.4% | 54.5% | 64.7% | 65.5% | nan% (0) | nan% (0) |
| codigo | 4,000,162 | 5,264,041 | 91.8% | 85.5% | 91.5% | 91.8% | 91.8% | nan% (0) | nan% (0) |
| MEZCLA | 24,009,306 | 101,559,459 | 73.6% | 42.2% | 51.7% | 62.5% | 72.3% | 17.8% (1,889) | 26.1% (433) |
