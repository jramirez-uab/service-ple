# Perfil de acceso a la tabla PLE

Fuentes: arxiv, codigo, wiki-ca, wiki-en, wiki-es, wiki-zh. Consultas = tokens × 16 filas.

## Concentración (la caché se llena y se prueba con la misma fuente: optimista)

| Fuente | Consultas | Filas distintas | top 1 M (0.2 GB) | top 4 M (0.7 GB) | top 16 M (2.7 GB) | top 64 M (10.7 GB) |
|---|---|---|---|---|---|---|
| arxiv | 322,252,224 | 67,990,685 | 51.1 % | 65.5 % | 80.9 % | 99.7 % |
| codigo | 324,313,440 | 14,543,526 | 87.1 % | 95.3 % | 100.0 % | 100.0 % |
| wiki-ca | 344,499,424 | 54,254,261 | 60.0 % | 74.0 % | 87.4 % | 100.0 % |
| wiki-en | 342,755,696 | 93,230,423 | 42.2 % | 55.1 % | 71.1 % | 92.4 % |
| wiki-es | 382,023,968 | 74,695,421 | 51.7 % | 65.9 % | 80.8 % | 98.0 % |
| wiki-zh | 355,260,480 | 115,244,526 | 35.0 % | 46.7 % | 62.4 % | 86.5 % |

## Sesgo cruzado: caché de 4 M filas (0.7 GB) llenada con la fila, probada con la columna

| caché \ prueba | arxiv | codigo | wiki-ca | wiki-en | wiki-es | wiki-zh |
|---|---|---|---|---|---|---|
| arxiv | 65.5 % | 26.0 % | 11.9 % | 30.6 % | 13.9 % | 7.5 % |
| codigo | 21.8 % | 95.3 % | 9.3 % | 23.3 % | 12.8 % | 7.7 % |
| wiki-ca | 10.5 % | 15.7 % | 74.0 % | 18.4 % | 29.7 % | 8.1 % |
| wiki-en | 32.9 % | 30.9 % | 16.8 % | 55.1 % | 19.8 % | 8.5 % |
| wiki-es | 10.4 % | 17.4 % | 26.4 % | 19.1 % | 65.9 % | 8.2 % |
| wiki-zh | 9.4 % | 18.3 % | 10.6 % | 16.8 % | 13.5 % | 46.7 % |
| MEZCLA | 51.7 % | 84.9 % | 60.4 % | 42.4 % | 51.7 % | 31.5 % |

## Sesgo cruzado: caché de 16 M filas (2.7 GB) llenada con la fila, probada con la columna

| caché \ prueba | arxiv | codigo | wiki-ca | wiki-en | wiki-es | wiki-zh |
|---|---|---|---|---|---|---|
| arxiv | 80.9 % | 33.4 % | 17.6 % | 39.2 % | 19.3 % | 11.4 % |
| codigo | 30.4 % | 100.0 % | 13.9 % | 30.4 % | 17.2 % | 11.6 % |
| wiki-ca | 17.3 % | 22.7 % | 87.4 % | 26.2 % | 39.0 % | 12.2 % |
| wiki-en | 43.6 % | 40.2 % | 24.0 % | 71.1 % | 26.7 % | 12.5 % |
| wiki-es | 17.4 % | 25.0 % | 35.6 % | 27.1 % | 80.8 % | 12.4 % |
| wiki-zh | 16.0 % | 24.9 % | 15.8 % | 23.8 % | 18.9 % | 62.4 % |
| MEZCLA | 67.1 % | 92.5 % | 74.7 % | 57.6 % | 67.2 % | 46.1 % |
