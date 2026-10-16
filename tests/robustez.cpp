// Robustez (2026-10-16): un caso por defecto encontrado en la revisión de código; cada uno fallaba (o colgaba) antes del arreglo.
// Uso: service-ple-test-robustez DIRECTORIO_TEMPORAL
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "cache.hpp"
#include "protocolo.hpp"
#include "tabla.hpp"

extern "C" {
void* sple_abrir(const char*, char*, int);
void sple_cerrar(void*);
}

using namespace sple;
static int fallos = 0;
static void comprobar(bool ok, const char* que) {
  std::printf("%s %s\n", ok ? "ok  " : "FALLO", que);
  if (!ok) ++fallos;
}

static void escribir_safetensors(const std::string& ruta, const std::string& json) {
  std::ofstream f(ruta, std::ios::binary);
  uint64_t n = json.size();
  f.write(reinterpret_cast<const char*>(&n), 8);
  f << json << std::string(4096, '\0');
}

static bool rechaza(const std::string& ruta) {
  try { leer_cabecera(ruta); return false; } catch (const std::exception&) { return true; }
}

// servidor TCP falso en 127.0.0.1: acepta y hace lo que diga `que` (0 = no contestar nunca, 1 = contestar basura)
static int servidor_falso(int que, int& puerto) {
  int s = ::socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t l = sizeof a;
  ::bind(s, reinterpret_cast<sockaddr*>(&a), sizeof a);
  ::listen(s, 4);
  ::getsockname(s, reinterpret_cast<sockaddr*>(&a), &l);
  puerto = ntohs(a.sin_port);
  std::thread([s, que] {
    int c = ::accept(s, nullptr, nullptr);
    char b[64];
    if (::recv(c, b, sizeof b, 0) > 0 && que == 1) { std::memset(b, 0x41, sizeof b); ::send(c, b, sizeof b, 0); }
    if (que == 2)  // goteo: un byte cada 300 ms, siempre dentro de la espera de cada recv
      for (int k = 0; k < 40 && ::send(c, b, 1, MSG_NOSIGNAL) == 1; ++k) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::this_thread::sleep_for(std::chrono::seconds(3));
    ::close(c);
    ::close(s);
  }).detach();
  return s;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "uso: %s DIRECTORIO_TEMPORAL\n", argv[0]); return 2; }
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // que se vea hasta dónde llega si algo revienta
  const std::string d = argv[1];

  // 1) caché: un agente que murió a medio escribir deja un hueco con versión impar; al reutilizar la caché se vacía
  const uint64_t huella = 0x5eed00000000abcdull;
  {
    auto k = Cache::crear(huella, 160, 64);
    uint8_t fila[160];
    std::memset(fila, 7, sizeof fila);
    k.escribir(5, fila);
    // hueco del id 5 con versión impar (escritura a medias)
    auto* base = reinterpret_cast<uint8_t*>(&k.cabecera()) + sizeof(CabeceraCache);
    bool marcado = false;
    for (uint64_t i = 0; i < k.capacidad(); ++i) {
      auto* h = reinterpret_cast<Hueco*>(base + (i / VIAS) * sizeof(Conjunto) + (i % VIAS) * sizeof(Hueco));
      if (h->id == 6) { h->version.fetch_add(1); marcado = true; }
    }
    comprobar(marcado, "caché: hueco del id 5 marcado como escritura a medias");
  }
  {
    auto k = Cache::crear(huella, 160, 64);
    uint8_t fila[160], leida[160];
    std::memset(fila, 9, sizeof fila);
    comprobar(k.reutilizada(), "caché: reutilizada tras la caída del agente");
    k.escribir(5, fila);
    comprobar(k.leer(5, leida) && leida[0] == 9, "caché: el id 5 se vuelve a escribir y se lee (antes quedaba muerto o se leía a medias)");
  }
  // 2) abrir: cabeceras de /dev/shm dañadas
  {
    Cache k;
    comprobar(Cache::abrir(huella, k), "caché: abrir la buena");
    k.cabecera().bytes_fila = 200;
    Cache k2;
    comprobar(!Cache::abrir(huella, k2), "caché: abrir rechaza bytes_fila > 160 (antes memcpy de 200 B sobre 160)");
    k.cabecera().bytes_fila = 160;
    uint64_t c = k.cabecera().conjuntos;
    k.cabecera().conjuntos = 0;
    comprobar(!Cache::abrir(huella, k2), "caché: abrir rechaza conjuntos = 0");
    k.cabecera().conjuntos = c;
  }
  ::unlink(ruta_cache(huella).c_str());

  // 2b) el agente se reinicia con otro --filas mientras un motor tiene la caché abierta: el motor no puede caerse (antes, ftruncate del mismo
  // fichero: con la caché más pequeña SIGBUS, con la más grande el motor indexaba con los conjuntos nuevos fuera de su mmap)
  for (uint64_t filas_nuevas : {uint64_t{16}, uint64_t{1} << 16}) {
    pid_t hijo = ::fork();
    if (hijo == 0) {
      { auto k = Cache::crear(huella, 160, 1024); }
      Cache motor;
      if (!Cache::abrir(huella, motor)) _exit(3);
      { auto k = Cache::crear(huella, 160, filas_nuevas); }
      if (motor.vigente()) _exit(4);  // tiene que notar que el fichero es otro para abrir el nuevo
      uint8_t fila[160];
      for (uint64_t id = 0; id < 100000; ++id) motor.leer(id, fila);
      _exit(0);
    }
    int st = 0;
    ::waitpid(hijo, &st, 0);
    ::unlink(ruta_cache(huella).c_str());
    comprobar(WIFEXITED(st) && WEXITSTATUS(st) == 0, filas_nuevas < 1024 ? "caché: el motor sigue vivo si el agente la recrea más pequeña"
                                                                       : "caché: el motor sigue vivo si el agente la recrea más grande");
  }

  // 3) cabecera safetensors dañada
  const std::string t = d + "/mala.safetensors";
  const std::string escala = R"("x.weight_scale": {"dtype": "BF16", "shape": [1], "data_offsets": [0, 2]})";
  escribir_safetensors(t, "{" + escala + R"(, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [1, 160], "data_offsets": [-1, 159]}})");
  comprobar(rechaza(t), "tabla: rechaza data_offsets negativos (antes desplazamiento enorme)");
  escribir_safetensors(t, "{" + escala + R"(, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [0, 160], "data_offsets": [2, 2]}})");
  comprobar(rechaza(t), "tabla: rechaza un trozo de 0 filas (antes división por cero en el agente)");
  escribir_safetensors(t, "{" + escala + R"(, "y": {"dtype": "BF16", "shape": [-], "data_offsets": [0, 2]}})");
  comprobar(rechaza(t), "tabla: un número no válido da error");
  escribir_safetensors(t, "{" + escala + R"(, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [1, 160], "data_offsets": [2, 162]}} basura)");
  comprobar(rechaza(t), "tabla: datos detrás del JSON");
  escribir_safetensors(t, "{" + escala + R"(, "y.weight_scale": {"dtype": "BF16", "shape": [1], "data_offsets": [0, 2]}, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [1, 160], "data_offsets": [2, 162]}})");
  comprobar(rechaza(t), "tabla: dos escalas (antes se usaba la última sin avisar)");
  escribir_safetensors(t, "{" + escala + R"(, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [100000, 160], "data_offsets": [2, 16000002]}})");
  comprobar(rechaza(t), "tabla: datos más allá del final del fichero");
  escribir_safetensors(t, "{" + escala + R"(, "x.shard_0.weight": {"dtype": "F8_E4M3", "shape": [1, 160], "data_offsets": [2, 162]}})");
  comprobar(!rechaza(t), "tabla: la buena se sigue aceptando");
  bool lanza = false;
  try { Cache::crear(huella, 160, uint64_t{1} << 62); } catch (const std::exception&) { lanza = true; }
  comprobar(lanza, "caché: --filas absurdo da error (antes bucle infinito)");

  // 4) cliente
  char e[512];
  std::string larga = "unix:/" + std::string(200, 'x');
  comprobar(!sple_abrir(larga.c_str(), e, sizeof e) && std::strstr(e, "demasiado larga"), "cliente: ruta de socket demasiado larga, con su error");
  int puerto = 0;
  ::setenv("SERVICE_PLE_ESPERA_MS", "0", 1);
  int s = servidor_falso(0, puerto);
  auto t0 = std::chrono::steady_clock::now();
  void* h = sple_abrir(("127.0.0.1:" + std::to_string(puerto)).c_str(), e, sizeof e);
  double seg = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  comprobar(!h && seg < 1.0, "cliente: SERVICE_PLE_ESPERA_MS=0 no deja colgado ante un servidor mudo (antes, sin límite)");
  ::unsetenv("SERVICE_PLE_ESPERA_MS");
  (void)s;
  servidor_falso(1, puerto);
  h = sple_abrir(("127.0.0.1:" + std::to_string(puerto)).c_str(), e, sizeof e);
  comprobar(!h && std::strstr(e, "no es de service-ple"), "cliente: rechaza una respuesta con otra magia");
  if (h) sple_cerrar(h);
  ::setenv("SERVICE_PLE_ESPERA_MS", "800", 1);
  servidor_falso(2, puerto);
  t0 = std::chrono::steady_clock::now();
  h = sple_abrir(("127.0.0.1:" + std::to_string(puerto)).c_str(), e, sizeof e);
  seg = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  comprobar(!h && seg < 3.0, "cliente: un servidor que gotea bytes no lo para más de 2 × espera (antes, 12 s o más)");
  ::unsetenv("SERVICE_PLE_ESPERA_MS");
  if (h) sple_cerrar(h);

  // la última: con el código anterior revienta la pila
  escribir_safetensors(t, "{\"a\": " + std::string(100000, '[') + "}");
  comprobar(rechaza(t), "tabla: anidamiento de 100.000 niveles rechazado (antes desbordaba la pila)");

  std::printf("RESULTADO: %s\n", fallos ? "FALLO" : "OK");
  return fallos ? 1 : 0;
}
