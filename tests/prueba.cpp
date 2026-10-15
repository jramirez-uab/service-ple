// service-ple: prueba del servidor. 1) filas al azar comparadas byte a byte con el fichero; 2) latencia de pasos de generación
// (128 filas = 8 sesiones × 16) y de trozos de prefill (32.768 filas = 2.048 tokens × 16).
// Uso: service-ple-prueba --servidor host:puerto [--fichero ple-fp8.safetensors] [--comprobar 200000]
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "tabla.hpp"

extern "C" {
void* sple_abrir(const char*, char*, int);
void sple_cerrar(void*);
int sple_info(void*, uint64_t*, uint32_t*, uint16_t*, uint64_t*);
int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int);
}

using Reloj = std::chrono::steady_clock;

int main(int argc, char** argv) {
  std::string servidor, fichero;
  uint64_t comprobar = 200000;
  for (int i = 1; i + 1 < argc; i += 2) {
    std::string a = argv[i];
    if (a == "--servidor") servidor = argv[i + 1];
    else if (a == "--fichero") fichero = argv[i + 1];
    else if (a == "--comprobar") comprobar = std::stoull(argv[i + 1]);
  }
  char err[512];
  void* h = sple_abrir(servidor.c_str(), err, sizeof err);
  if (!h) { std::printf("FALLO: %s\n", err); return 1; }
  uint64_t filas, huella; uint32_t bf; uint16_t escala;
  sple_info(h, &filas, &bf, &escala, &huella);
  std::printf("servidor %s: %lu filas × %u B, escala 0x%04x, huella %016lx\n", servidor.c_str(), filas, bf, escala, huella);
  std::mt19937_64 rng(12345);
  int fallos = 0;

  if (!fichero.empty()) {
    auto d = sple::leer_cabecera(fichero);
    bool igual = d.huella() == huella && d.escala_bf16 == escala && d.filas == filas && d.bytes_fila == bf;
    std::printf("cabecera local: %s\n", igual ? "igual (huella, escala, filas)" : "DISTINTA");
    if (!igual) ++fallos;
    int fd = ::open(fichero.c_str(), O_RDONLY);
    std::uniform_int_distribution<int64_t> u(0, static_cast<int64_t>(filas) - 1);
    std::vector<int64_t> ids(4096);
    std::vector<uint8_t> out, ref(bf);
    uint64_t hechas = 0, distintas = 0;
    while (hechas < comprobar) {
      for (auto& x : ids) x = u(rng);
      ids[0] = 0; ids[1] = static_cast<int64_t>(filas) - 1; ids[2] = ids[3];  // extremos y un repetido
      out.assign(ids.size() * bf, 0xAA);
      if (sple_filas(h, ids.data(), ids.size(), 0, static_cast<int64_t>(filas), out.data(), err, sizeof err)) { std::printf("FALLO: %s\n", err); return 1; }
      for (size_t i = 0; i < ids.size(); ++i) {
        uint64_t t = static_cast<uint64_t>(ids[i]) / d.filas_por_trozo, f = static_cast<uint64_t>(ids[i]) % d.filas_por_trozo;
        if (::pread(fd, ref.data(), bf, static_cast<off_t>(d.trozos[t].desplazamiento + f * bf)) != static_cast<ssize_t>(bf)) { std::printf("FALLO leyendo el fichero\n"); return 1; }
        if (std::memcmp(ref.data(), out.data() + i * bf, bf) != 0) ++distintas;
      }
      hechas += ids.size();
    }
    // rango parcial (TP): fuera de [lo, hi) deben salir ceros
    int64_t lo = static_cast<int64_t>(filas) / 2;
    out.assign(ids.size() * bf, 0xAA);
    sple_filas(h, ids.data(), ids.size(), lo, static_cast<int64_t>(filas), out.data(), err, sizeof err);
    uint64_t ceros_mal = 0;
    for (size_t i = 0; i < ids.size(); ++i)
      if (ids[i] < lo) ceros_mal += std::any_of(out.begin() + i * bf, out.begin() + (i + 1) * bf, [](uint8_t b) { return b != 0; });
    std::printf("comprobadas %lu filas al azar: %lu distintas; rango parcial: %lu filas sin ceros\n", hechas, distintas, ceros_mal);
    if (distintas || ceros_mal) ++fallos;
    ::close(fd);
  }

  auto medir = [&](const char* nombre, size_t n, int veces) {
    std::uniform_int_distribution<int64_t> u(0, static_cast<int64_t>(filas) - 1);
    std::vector<int64_t> ids(n);
    std::vector<uint8_t> out(n * bf);
    std::vector<double> t;
    for (int v = 0; v < veces; ++v) {
      for (auto& x : ids) x = u(rng);
      auto t0 = Reloj::now();
      if (sple_filas(h, ids.data(), n, 0, static_cast<int64_t>(filas), out.data(), err, sizeof err)) { std::printf("FALLO: %s\n", err); std::exit(1); }
      t.push_back(std::chrono::duration<double, std::milli>(Reloj::now() - t0).count());
    }
    std::ranges::sort(t);
    double media = 0; for (double x : t) media += x; media /= t.size();
    std::printf("%s (%zu filas, %.1f KB): mediana %.3f ms, p99 %.3f ms, media %.3f ms → %.1f MB/s\n", nombre, n, n * bf / 1e3,
                t[t.size() / 2], t[t.size() * 99 / 100], media, n * bf / (media / 1e3) / 1e6);
  };
  medir("paso de generación, 8 sesiones", 128, 2000);
  medir("paso de generación, 1 sesión", 16, 2000);
  medir("trozo de prefill de 2.048 tokens", 32768, 40);
  sple_cerrar(h);
  std::printf(fallos ? "RESULTADO: FALLO\n" : "RESULTADO: OK\n");
  return fallos ? 1 : 0;
}
