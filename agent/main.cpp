// service-ple fase 2: agente local por máquina. Mantiene la caché compartida (/dev/shm/service-ple-<huella>) que leen directamente los
// motores, y atiende por un socket Unix, con el mismo protocolo que el servidor, las filas que les faltan: las pide al servidor (una sola
// conexión por máquina), las guarda en la caché y las devuelve. Es el único que escribe en la caché.
// Si se le da --fichero (el ple-fp8.safetensors local), es el respaldo cuando el servidor falla: tras --fallos-para-disco fallos de red
// seguidos (10) pasa a leer las filas del disco (misma huella comprobada) y solo vuelve a probar la red cada --reintento-red s (30).
// Antes de llegar al umbral, o sin fichero, contesta con error y el motor sigue con ceros.
// Sin --servidor (solo --fichero): agente solo local, la caché compartida delante y el disco detrás, sin red.
// Uso: service-ple-agent [--servidor host:puerto] [--socket /run/service-ple/agente.sock] [--filas 16777216] [--fichero ple-fp8.safetensors] [--hilos-disco 16] [--fallos-para-disco 10] [--reintento-red 30]
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>

#include "cache.hpp"
#include "tabla.hpp"
#include "formato.hpp"
#include "protocolo.hpp"

extern "C" {
void* sple_abrir(const char*, char*, int);
int sple_info(void*, uint64_t*, uint32_t*, uint16_t*, uint64_t*);
int sple_filas(void*, const int64_t*, uint64_t, int64_t, int64_t, uint8_t*, char*, int);
void sple_cerrar(void*);
}

namespace {

using namespace sple;

// no lanza: se llama desde los catch de los hilos, y un bad_alloc aquí (sin memoria) terminaría el proceso entero
template <class... A>
void log(const char* f, A&&... a) noexcept {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char hora[32];
  std::strftime(hora, sizeof hora, "%F %T", &tm);
  try {
    std::fputs((std::string(hora) + " " + sple::formato(f, std::forward<A>(a)...) + "\n").c_str(), stdout);
  } catch (...) {
    std::fprintf(stdout, "%s (mensaje perdido: sin memoria)\n", hora);
  }
  std::fflush(stdout);
}

bool leer_todo(int fd, void* buf, size_t n) {
  auto* p = static_cast<uint8_t*>(buf);
  while (n) {
    ssize_t r = ::recv(fd, p, n, 0);
    if (r <= 0) return false;
    p += r; n -= static_cast<size_t>(r);
  }
  return true;
}

bool enviar_todo(int fd, const void* buf, size_t n) {
  auto* p = static_cast<const uint8_t*>(buf);
  while (n) {
    ssize_t r = ::send(fd, p, n, MSG_NOSIGNAL);
    if (r <= 0) return false;
    p += r; n -= static_cast<size_t>(r);
  }
  return true;
}

// Respaldo en disco: lecturas sueltas de una fila (pread) repartidas entre varios hilos
struct Disco {
  int fd = -1;
  std::string ruta;
  DescTabla d;
  int hilos = 16;
  std::atomic<uint64_t> filas_leidas{0};
  // huella_servidor = 0: sin servidor (agente solo local), se acepta la tabla del fichero
  bool abrir(const std::string& fichero, uint64_t huella_servidor) {
    d = leer_cabecera(fichero);
    if (huella_servidor && d.huella() != huella_servidor)
      throw std::runtime_error(sple::formato("{} es otra tabla (huella {:016x}, el servidor {:016x})", fichero, d.huella(), huella_servidor));
    fd = ::open(fichero.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) throw std::runtime_error("no se puede abrir " + fichero);
    ruta = fichero;
    ::posix_fadvise(fd, 0, 0, POSIX_FADV_RANDOM);
    return true;
  }
  explicit operator bool() const { return fd >= 0; }
  bool leer(const std::vector<int64_t>& ids, uint8_t* out) {
    const uint32_t bf = d.bytes_fila;
    std::atomic<bool> ok{true};
    std::atomic<size_t> sig{0};
    auto trabajo = [&] {
      for (size_t k; ok && (k = sig.fetch_add(64)) < ids.size();)
        for (size_t i = k; i < std::min(k + 64, ids.size()); ++i) {
          uint64_t id = static_cast<uint64_t>(ids[i]), t = id / d.filas_por_trozo, f = id % d.filas_por_trozo;
          if (t >= d.trozos.size() || ::pread(fd, out + i * bf, bf, static_cast<off_t>(d.trozos[t].desplazamiento + f * bf)) != static_cast<ssize_t>(bf)) { ok = false; break; }
        }
    };
    int n = static_cast<int>(std::min<size_t>(hilos, (ids.size() + 63) / 64));
    if (n <= 1) trabajo();
    else { std::vector<std::jthread> h; for (int i = 0; i < n; ++i) h.emplace_back(trabajo); }
    if (ok) filas_leidas += ids.size();
    return ok;
  }
};

struct Agente {
  Disco disco;
  std::string direccion;  // servidor configurado (vacío = agente solo local)
  // modo disco: tras `fallos_para_disco` fallos de red seguidos; mientras dura, la red solo se prueba cada `reintento_red`
  int fallos_para_disco = 10;
  std::chrono::seconds reintento_red{30};
  std::mutex mx_modo;
  int fallos_seguidos = 0;
  bool modo_disco = false;
  std::chrono::steady_clock::time_point desde{}, proximo_intento_red{};
  std::atomic<void*> servidor{nullptr};  // lo pone un hilo bajo mx_modo y lo leen todos sin cerrojo
  Info info{};
  Cache cache;
  std::mutex mx_escritor;  // un solo escritor en la caché
  std::atomic<uint64_t> peticiones{0}, servidas_cache{0}, pedidas_servidor{0}, fallos_servidor{0};

  // filas que faltan: red, o disco si está en modo disco
  bool obtener(const std::vector<int64_t>& ids, uint8_t* out) {
    using namespace std::chrono;
    auto ahora = steady_clock::now();
    if (!servidor) {
      // sin conexión al servidor: solo local (sin dirección) o servidor caído desde el arranque (se reintenta cada reintento_red)
      std::lock_guard g(mx_modo);
      if (!direccion.empty() && ahora >= proximo_intento_red) {
        proximo_intento_red = ahora + reintento_red;
        char e[512];
        if (void* h = sple_abrir(direccion.c_str(), e, sizeof e)) {
          Info i{};
          sple_info(h, &i.filas, &i.bytes_fila, &i.escala_bf16, &i.huella);
          if (i.huella == info.huella) { servidor.store(h); log("servidor {} disponible: se deja el disco", direccion); }
          else { log("el servidor {} sirve otra tabla ({:016x}); se sigue con el disco", direccion, i.huella); sple_cerrar(h); }
        }
      }
      if (!servidor) return disco.leer(ids, out);
    }
    bool probar_red;
    {
      std::lock_guard g(mx_modo);
      probar_red = !modo_disco || ahora >= proximo_intento_red;
      if (modo_disco && probar_red) proximo_intento_red = ahora + reintento_red;
    }
    char err[512] = "";
    if (probar_red && sple_filas(servidor.load(), ids.data(), ids.size(), 0, static_cast<int64_t>(info.filas), out, err, sizeof err) == 0) {
      pedidas_servidor += ids.size();
      std::lock_guard g(mx_modo);
      if (modo_disco) log("la red vuelve a responder tras {} s en modo disco ({} filas leídas del disco); se deja el disco",
                          duration_cast<seconds>(ahora - desde).count(), disco.filas_leidas.load());
      else if (fallos_seguidos) log("la red vuelve a responder tras {} fallos seguidos", fallos_seguidos);
      modo_disco = false;
      fallos_seguidos = 0;
      return true;
    }
    {
      std::lock_guard g(mx_modo);
      if (probar_red) {
        ++fallos_servidor;
        if (++fallos_seguidos == 1) log("sin servidor ({})", err);
        if (!modo_disco && disco && fallos_seguidos >= fallos_para_disco) {
          modo_disco = true;
          desde = ahora;
          proximo_intento_red = ahora + reintento_red;
          log("{} fallos de red seguidos: se pasa al disco local ({}); la red se reintenta cada {} s", fallos_seguidos, disco.ruta, reintento_red.count());
        }
      }
      if (!modo_disco) return false;
    }
    return disco.leer(ids, out);
  }

  void atender(int fd) {
    std::vector<uint32_t> ids, faltan;
    std::vector<size_t> pos;
    std::vector<int64_t> ids64;
    std::vector<uint8_t> salida, recibidas;
    const uint32_t bf = info.bytes_fila;
    for (Cabecera c; leer_todo(fd, &c, sizeof c);) {
      Cabecera r{MAGIA, VERSION, c.op, 0, OK};
      if (c.magia != MAGIA || c.version != VERSION) { r.estado = c.magia != MAGIA ? ERR_MAGIA : ERR_VERSION; enviar_todo(fd, &r, sizeof r); break; }
      if (c.op == OP_INFO) {
        if (!enviar_todo(fd, &r, sizeof r) || !enviar_todo(fd, &info, sizeof info)) break;
        continue;
      }
      if (c.op != OP_FILAS || c.n > MAX_FILAS_PETICION) { r.estado = c.op != OP_FILAS ? ERR_OP : ERR_DEMASIADAS; enviar_todo(fd, &r, sizeof r); break; }
      ids.resize(c.n);
      if (!leer_todo(fd, ids.data(), c.n * sizeof(uint32_t))) break;
      ++peticiones;
      salida.resize(sizeof r + static_cast<size_t>(c.n) * bf);
      uint8_t* q = salida.data() + sizeof r;
      faltan.clear(); pos.clear();
      bool fuera = false;
      for (size_t k = 0; k < ids.size(); ++k) {
        if (ids[k] >= info.filas) { fuera = true; break; }
        if (!cache.leer(ids[k], q + k * bf)) { faltan.push_back(ids[k]); pos.push_back(k); }
      }
      if (fuera) { r.estado = ERR_FUERA_DE_RANGO; enviar_todo(fd, &r, sizeof r); break; }
      servidas_cache += ids.size() - faltan.size();
      if (!faltan.empty()) {
        ids64.assign(faltan.begin(), faltan.end());
        recibidas.resize(faltan.size() * bf);
        if (!obtener(ids64, recibidas.data())) {
          // ni servidor ni disco (o aún por debajo del umbral): el motor sigue con ceros en este paso; la conexión sigue abierta
          r.estado = ERR_SIN_SERVIDOR;
          if (!enviar_todo(fd, &r, sizeof r)) break;
          continue;
        }
        std::lock_guard g(mx_escritor);
        for (size_t f = 0; f < faltan.size(); ++f) {
          std::memcpy(q + pos[f] * bf, recibidas.data() + f * bf, bf);
          cache.escribir(faltan[f], recibidas.data() + f * bf);
        }
      }
      r.n = c.n;
      std::memcpy(salida.data(), &r, sizeof r);
      if (!enviar_todo(fd, salida.data(), salida.size())) break;
    }
    ::close(fd);
  }
};

}  // namespace

int main(int argc, char** argv) {
  std::string servidor, socket_ruta = "/run/service-ple/agente.sock";
  uint64_t filas = 16ull << 20;
  std::string fichero;
  int hilos_disco = 16, fallos_para_disco = 10, reintento_red = 30;
  for (int i = 1; i < argc; i += 2) {
    std::string a = argv[i];
    if (i + 1 >= argc) { std::fprintf(stderr, "falta el valor de %s\n", a.c_str()); return 2; }
    // un valor no numérico es un error de uso, no un std::terminate
    auto num = [&](uint64_t max) -> uint64_t {
      std::string v = argv[i + 1];
      try { size_t f; uint64_t n = std::stoull(v, &f); if (f == v.size() && n > 0 && n <= max) return n; } catch (const std::exception&) {}
      std::fprintf(stderr, "valor no válido para %s: %s\n", a.c_str(), v.c_str()); std::exit(2);
    };
    if (a == "--servidor") servidor = argv[i + 1];
    else if (a == "--socket") socket_ruta = argv[i + 1];
    else if (a == "--filas") filas = num(uint64_t{1} << 36);
    else if (a == "--fichero") fichero = argv[i + 1];
    else if (a == "--hilos-disco") hilos_disco = static_cast<int>(num(1024));
    else if (a == "--fallos-para-disco") fallos_para_disco = static_cast<int>(num(1u << 20));
    else if (a == "--reintento-red") reintento_red = static_cast<int>(num(86400));
    else { std::fprintf(stderr, "uso: %s --servidor host:puerto [--socket ruta] [--filas N] [--fichero ple-fp8.safetensors] [--hilos-disco N]\n", argv[0]); return 2; }
  }
  if (servidor.empty() && fichero.empty()) { std::fprintf(stderr, "hace falta --servidor, --fichero o los dos\n"); return 2; }
  ::setenv("SERVICE_PLE_DEGRADAR", "0", 1);  // el agente no se inventa filas: si el servidor falla, lo dice
  std::signal(SIGPIPE, SIG_IGN);
  try {
    Agente ag;
    ag.direccion = servidor;
    char err[512];
    if (!servidor.empty()) {
      // con fichero de respaldo no hace falta esperar al servidor para arrancar: si no está, se empieza en modo disco
      for (int intento = 0; !(ag.servidor = sple_abrir(servidor.c_str(), err, sizeof err)) && fichero.empty(); ++intento) {
        if (intento % 12 == 0) log("esperando al servidor {}: {}", servidor, err);
        std::this_thread::sleep_for(std::chrono::seconds(5));
      }
    }
    if (ag.servidor) {
      sple_info(ag.servidor, &ag.info.filas, &ag.info.bytes_fila, &ag.info.escala_bf16, &ag.info.huella);
    } else {
      DescTabla d = leer_cabecera(fichero);
      ag.info = Info{d.filas, d.bytes_fila, d.escala_bf16, 0, d.huella()};
      if (servidor.empty()) log("sin servidor configurado: agente solo local, tabla {} desde el disco", fichero);
      else log("servidor {} no disponible al arrancar ({}): se empieza solo con el disco", servidor, err);
    }
    ag.cache = Cache::crear(ag.info.huella, ag.info.bytes_fila, filas);
    if (!fichero.empty()) {
      ag.disco.hilos = hilos_disco;
      ag.fallos_para_disco = fallos_para_disco;
      ag.reintento_red = std::chrono::seconds(reintento_red);
      ag.disco.abrir(fichero, ag.servidor ? ag.info.huella : 0);
      log("respaldo en disco: {} (misma tabla; {} hilos de lectura; tras {} fallos de red seguidos; red reintentada cada {} s)", fichero, hilos_disco,
          fallos_para_disco, reintento_red);
    }
    log("tabla {:016x} ({} filas × {} B) desde {}; caché {} en {} filas ({:.1f} GB){}", ag.info.huella, ag.info.filas, ag.info.bytes_fila,
        ag.servidor ? servidor : fichero,
        ruta_cache(ag.info.huella), ag.cache.capacidad(), ag.cache.capacidad() * sizeof(Hueco) / 1e9, ag.cache.reutilizada() ? ", reutilizada" : ", nueva");

    if (socket_ruta.size() >= sizeof(sockaddr_un::sun_path)) throw std::runtime_error("ruta de socket demasiado larga: " + socket_ruta);
    auto barra = socket_ruta.rfind('/');
    if (barra != std::string::npos) ::mkdir(socket_ruta.substr(0, barra).c_str(), 0755);
    ::unlink(socket_ruta.c_str());
    int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un dir{};
    dir.sun_family = AF_UNIX;
    std::snprintf(dir.sun_path, sizeof dir.sun_path, "%s", socket_ruta.c_str());
    if (::bind(s, reinterpret_cast<sockaddr*>(&dir), sizeof dir) != 0 || ::listen(s, 64) != 0)
      throw std::runtime_error("no se puede escuchar en " + socket_ruta + ": " + std::strerror(errno));
    ::chmod(socket_ruta.c_str(), 0660);
    log("agente listo en {}", socket_ruta);

    std::jthread informe([&](std::stop_token st) {
      auto& cab = ag.cache.cabecera();
      uint64_t a0 = 0, f0 = 0;
      while (!st.stop_requested()) {
        for (int i = 0; i < 600 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t a = cab.aciertos.load() + ag.servidas_cache.load(), f = ag.pedidas_servidor.load();
        if (a + f != a0 + f0)
          log("último minuto: {} filas desde la caché, {} del servidor ({:.0f} % de aciertos); caché con {} insertadas en total; {} leídas del disco",
              a - a0, f - f0, 100.0 * (a - a0) / double(a - a0 + f - f0), cab.insertadas.load(), ag.disco.filas_leidas.load());
        a0 = a; f0 = f;
      }
    });
    for (;;) {
      int c = ::accept4(s, nullptr, nullptr, SOCK_CLOEXEC);
      if (c < 0) {
        // sin descriptores o memoria (EMFILE, ENFILE, ENOBUFS, ENOMEM) o conexión abortada: se espera y se sigue, no se cae el servicio
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if (errno == EMFILE || errno == ENFILE || errno == ENOBUFS || errno == ENOMEM) {
          log("accept: {}; se reintenta en 100 ms", std::strerror(errno));
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        throw std::runtime_error(std::string("accept: ") + std::strerror(errno));
      }
      try {
        std::thread([&ag, c] {
          try { ag.atender(c); } catch (const std::exception& ex) { log("conexión local: error ({}); se cierra", ex.what()); ::close(c); }  // no tirar el agente
        }).detach();
      } catch (const std::exception& ex) {  // sin hilos (system_error) o sin memoria: se rechaza esta conexión, no se cae el agente
        log("no se puede crear un hilo para una conexión local ({}); se cierra", ex.what());
        ::close(c);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
    }
  } catch (const std::exception& e) {
    log("ERROR: {}", e.what());
    return 1;
  }
}
