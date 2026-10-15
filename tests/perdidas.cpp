#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <vector>
extern "C" { void* sple_abrir(const char*, char*, int); int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int); }
int main(int, char** a) {
  char e[256]; void* h = sple_abrir(a[1], e, 256); if (!h) { puts(e); return 1; }
  int64_t filas = atoll(a[2]); std::vector<int64_t> ids(filas); for (int64_t i = 0; i < filas; ++i) ids[i] = i;
  std::vector<uint8_t> out(filas * 160); sple_filas(h, ids.data(), filas, 0, filas, out.data(), e, 256);
  long ceros = 0; for (int64_t i = 0; i < filas; ++i) { bool z = true; for (int k = 0; k < 160; ++k) z &= out[i*160+k] == 0; ceros += z; }
  printf("filas a cero: %.2f %%\n", 100.0 * ceros / filas);
}
