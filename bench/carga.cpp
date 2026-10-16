// service-ple: generador de carga. N conexiones (un hilo y un manejador cada una) piden sin pausa F filas al azar durante S segundos.
// Uso: service-ple-carga destino conexiones filas_por_peticion segundos [rango]  (rango: ids en [0, rango); por defecto toda la tabla)
// Se compila con el resto (build/service-ple-carga).
// Salida: peticiones/s, filas/s, MB/s y latencia p50/p99/máx de una petición (ms).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <thread>
#include <vector>

extern "C" {
void* sple_abrir(const char*, char*, int);
int sple_info(void*, uint64_t*, uint32_t*, uint16_t*, uint64_t*);
int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int);
void sple_cerrar(void*);
}

int main(int argc, char** argv) {
  if (argc != 5 && argc != 6) { std::fprintf(stderr, "uso: %s destino conexiones filas_por_peticion segundos [rango]\n", argv[0]); return 2; }
  const char* destino = argv[1];
  const int conexiones = std::atoi(argv[2]);
  const uint64_t f = std::strtoull(argv[3], nullptr, 10);
  const int segundos = std::atoi(argv[4]);
  if (conexiones < 1 || f == 0 || segundos < 1) { std::fprintf(stderr, "conexiones, filas y segundos: al menos 1\n"); return 2; }
  std::vector<void*> h(conexiones);
  uint64_t filas = 0;
  uint32_t bf = 0;
  for (int i = 0; i < conexiones; ++i) {
    char e[512];
    if (!(h[i] = sple_abrir(destino, e, sizeof e))) { std::fprintf(stderr, "conexión %d: %s\n", i, e); return 1; }
    uint16_t esc; uint64_t hu;
    sple_info(h[i], &filas, &bf, &esc, &hu);
  }
  const uint64_t rango = argc == 6 ? std::min<uint64_t>(std::strtoull(argv[5], nullptr, 10), filas) : filas;
  if (rango == 0) { std::fprintf(stderr, "rango: al menos 1\n"); return 2; }
  std::atomic<bool> parar{false};
  std::atomic<uint64_t> errores{0};
  std::vector<std::vector<double>> lat(conexiones);
  std::vector<std::thread> t;
  for (int i = 0; i < conexiones; ++i)
    t.emplace_back([&, i] {
      std::mt19937_64 g(1234 + i);
      std::uniform_int_distribution<int64_t> d(0, static_cast<int64_t>(rango) - 1);
      std::vector<int64_t> ids(f);
      std::vector<uint8_t> out(f * bf);
      char e[512];
      while (!parar) {
        for (auto& x : ids) x = d(g);
        auto t0 = std::chrono::steady_clock::now();
        if (sple_filas(h[i], ids.data(), f, 0, static_cast<int64_t>(filas), out.data(), e, sizeof e)) ++errores;
        lat[i].push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
      }
    });
  std::this_thread::sleep_for(std::chrono::seconds(segundos));
  parar = true;
  for (auto& x : t) x.join();
  std::vector<double> todas;
  for (auto& v : lat) todas.insert(todas.end(), v.begin(), v.end());
  std::sort(todas.begin(), todas.end());
  const double n = static_cast<double>(todas.size());
  auto pct = [&](double p) { return todas.empty() ? 0.0 : todas[static_cast<size_t>(p * (n - 1))]; };
  std::printf("conexiones %d  filas/petición %lu  peticiones/s %.0f  filas/s %.0f  MB/s %.0f  ms p50 %.2f p99 %.2f máx %.1f  errores %lu\n",
              conexiones, static_cast<unsigned long>(f), n / segundos, n * f / segundos, n * f * bf / segundos / 1e6, pct(0.5), pct(0.99),
              todas.empty() ? 0.0 : todas.back(), static_cast<unsigned long>(errores.load()));
  for (auto x : h) sple_cerrar(x);
}
