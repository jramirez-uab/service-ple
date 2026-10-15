// pide 128 filas cada 50 ms durante N s; cada segundo: pasos, cuántos con filas buenas, peor latencia
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>
extern "C" { void sple_cerrar(void*); void* sple_abrir(const char*, char*, int); int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int); }
int main(int, char** a) {
  char e[256]; void* h = sple_abrir(a[1], e, 256); if (!h) { std::printf("abrir: %s\n", e); return 1; }
  int64_t filas = std::atoll(a[2]); int seg = std::atoi(a[3]);
  std::vector<int64_t> ids(128); std::vector<uint8_t> out(128 * 160);
  auto t0 = std::chrono::steady_clock::now();
  for (int s = 0; s < seg; ++s) {
    int pasos = 0, buenos = 0, errores = 0; double peor = 0;
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(s + 1)) {
      for (int i = 0; i < 128; ++i) ids[i] = (std::rand() * 7919LL) % filas;
      auto t = std::chrono::steady_clock::now();
      int r = sple_filas(h, ids.data(), 128, 0, filas, out.data(), e, 256);
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
      if (ms > peor) peor = ms;
      bool cero = true; for (auto b : out) if (b) { cero = false; break; }
      ++pasos; if (r) ++errores; else if (!cero) ++buenos;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("t=%2d s: %2d pasos, %2d con filas, %d errores, peor %.0f ms\n", s + 1, pasos, buenos, errores, peor); std::fflush(stdout);
  }
  sple_cerrar(h);
}
