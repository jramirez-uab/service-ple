// El motor arranca antes que el agente (conecta al servidor de respaldo) y tiene que pasarse al agente cuando aparece: si no, sus filas nunca
// llenan la caché de la máquina. Pide filas durante N s; al final, las filas que el agente metió en la caché tienen que ser > 0.
// Uso: service-ple-test-preferido "unix:/ruta;host:puerto" segundos
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "cache.hpp"

extern "C" {
void* sple_abrir(const char*, char*, int);
void sple_cerrar(void*);
int sple_info(void*, uint64_t*, uint32_t*, uint16_t*, uint64_t*);
int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int);
}

int main(int, char** a) {
  char e[512];
  void* h = sple_abrir(a[1], e, sizeof e);
  if (!h) { std::printf("abrir: %s\nRESULTADO: FALLO\n", e); return 1; }
  uint64_t filas, huella; uint32_t bf; uint16_t esc;
  sple_info(h, &filas, &bf, &esc, &huella);
  std::vector<int64_t> ids(64); std::vector<uint8_t> out(64 * bf);
  auto t0 = std::chrono::steady_clock::now();
  for (int64_t k = 0; std::chrono::steady_clock::now() - t0 < std::chrono::seconds(std::atoi(a[2])); ++k) {
    for (int i = 0; i < 64; ++i) ids[i] = (k * 64 + i) % static_cast<int64_t>(filas);
    sple_filas(h, ids.data(), 64, 0, static_cast<int64_t>(filas), out.data(), e, sizeof e);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  sple_cerrar(h);
  sple::Cache c;
  uint64_t ins = sple::Cache::abrir(huella, c) ? c.cabecera().insertadas.load() : 0;
  std::printf("filas insertadas por el agente: %llu\nRESULTADO: %s\n", static_cast<unsigned long long>(ins), ins ? "OK" : "FALLO");
  return ins ? 0 : 1;
}
