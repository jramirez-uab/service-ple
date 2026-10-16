// service-ple: biblioteca cliente (fase 1) con interfaz C, para cualquier motor (ctypes desde Python, enlace directo desde C++).
// Una conexión TCP persistente por manejador; una petición por llamada, con los ids del rango [lo, hi) ordenados y sin repetir.
// Las posiciones fuera de rango se rellenan con ceros (así funciona el reparto por vocabulario con TP: cada rango tiene un dueño).
// Robustez (2026-10-15): si el servidor no responde, el paso sigue con ceros (perder hasta un 5 % de filas sube la NLL un 0,1 %; sin PLE,
// un 3 %), reconecta solo y avisa en stderr al caer, cada minuto mientras dura y al volver. Variables: SERVICE_PLE_ESPERA_MS (espera máxima
// de una respuesta del servidor, 2000), SERVICE_PLE_ESPERA_LOCAL_MS (la del agente local, 30000: lee del disco las filas que no tiene, y una
// petición grande con un disco lento pasa de 2 s; con 2 s esas filas iban con ceros), SERVICE_PLE_CONEXION_MS (espera máxima al conectar,
// 300), SERVICE_PLE_DEGRADAR=0 (fallar en vez de ceros).
#include <dlfcn.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <vector>

#include "cache.hpp"
#include "protocolo.hpp"

namespace {

using namespace sple;
using Reloj = std::chrono::steady_clock;

// entero de una variable de entorno en [min, max]; si falta o no es un número, el valor por defecto (atoi daba 0 con «abc»: esperas de 1 ms)
int entero_env(const char* nombre, int defecto, int min = 0, int max = 600000) {
  const char* e = std::getenv(nombre);
  if (!e || !*e) return defecto;
  char* fin = nullptr;
  errno = 0;
  long v = std::strtol(e, &fin, 10);
  if (errno || *fin || v < min || v > max) {
    std::fprintf(stderr, "service-ple: %s=%s no es válido (de %d a %d); se usa %d\n", nombre, e, min, max, defecto);
    return defecto;
  }
  return static_cast<int>(v);
}

void aviso(const std::string& m) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char hora[32];
  std::strftime(hora, sizeof hora, "%F %T", &tm);
  std::fprintf(stderr, "%s service-ple: %s\n", hora, m.c_str());
  std::fflush(stderr);
}

struct Conexion {
  std::vector<std::string> destinos;  // en orden de preferencia: "unix:/ruta" (agente local) o "host:puerto" (servidor)
  std::string host, puerto;           // destino en uso (para los avisos)
  int fd = -1;
  Cache cache;                        // caché compartida del agente local, si existe para esta tabla
  std::vector<uint32_t> faltan;
  std::vector<size_t> pos_faltan;
  std::vector<uint8_t> recibidas;
  Info info{};
  std::mutex mx;
  std::vector<uint32_t> ids;
  std::vector<uint8_t> filas;
  std::string error;
  // robustez
  // al menos 1 ms: un timeval {0, 0} en SO_RCVTIMEO es «sin límite» y un servidor colgado pararía el motor para siempre
  // cada espera, como mucho 10 min: así 2 × espera_ms (tope total de un envío o una respuesta) no desborda
  const int espera_ms = entero_env("SERVICE_PLE_ESPERA_MS", 2000, 1);
  const int espera_local_ms = entero_env("SERVICE_PLE_ESPERA_LOCAL_MS", 30000, 1);
  int espera() const { return host.starts_with("unix:") ? espera_local_ms : espera_ms; }  // la del destino en uso
  const int conexion_ms = entero_env("SERVICE_PLE_CONEXION_MS", 300, 1);
  const bool degradar = entero_env("SERVICE_PLE_DEGRADAR", 1) != 0;
  uint64_t huella0 = 0;  // la tabla del primer INFO: al reconectar, el servidor tiene que dar la misma
  bool caido = false;
  Reloj::time_point caido_desde{}, proximo_intento{}, ultimo_aviso{};
  uint64_t pasos_ceros = 0, filas_ceros = 0;
  bool agotado = false;  // el último fallo fue por tiempo (servidor colgado): no reintentar en el acto
  Reloj::time_point proxima_cache{};      // sin caché del agente: se vuelve a buscar cada 30 s (el agente puede arrancar después que el motor)
  size_t usado = 0;                       // índice en destinos del que está en uso
  Reloj::time_point proximo_preferido{};  // en un destino de respaldo: cada 30 s se prueba el primero (el agente puede haber vuelto)

  ~Conexion() { cerrar(); }
  void cerrar() { if (fd >= 0) ::close(fd); fd = -1; }

  // prueba los destinos en orden
  bool conectar() {
    std::string errores;
    for (size_t i = 0; i < destinos.size(); ++i) {
      if (!elegir(destinos[i])) continue;
      if (conectar_uno()) {
        usado = i;
        proximo_preferido = Reloj::now() + std::chrono::seconds(30);
        if (!cache) abrir_cache();  // la del agente de esta máquina, si hay
        return true;
      }
      errores += (errores.empty() ? "" : "; ") + error;
    }
    error = errores;
    return false;
  }

  bool elegir(const std::string& d) {
    if (d.starts_with("unix:")) { host = d; puerto = ""; return true; }
    auto p = d.rfind(':');
    if (p == std::string::npos) { error = "dirección sin puerto: " + d; return false; }
    host = d.substr(0, p); puerto = d.substr(p + 1);
    if (!host.empty() && host.front() == '[') host = host.substr(1, host.size() - 2);
    return true;
  }

  // en un destino de respaldo (p. ej. el servidor porque el agente no estaba al arrancar): volver al preferido en cuanto responda, sin
  // soltar la conexión actual si no responde (si no, las filas que faltan nunca pasan por el agente ni llenan la caché de la máquina)
  void volver_al_preferido() {
    proximo_preferido = Reloj::now() + std::chrono::seconds(30);
    const int viejo = fd;
    const std::string h = host, p = puerto, e = error;
    fd = -1;
    if (elegir(destinos[0]) && conectar_uno()) {
      if (viejo >= 0) ::close(viejo);
      usado = 0;
      if (!cache) abrir_cache();
      return;
    }
    cerrar();
    fd = viejo; host = h; puerto = p; error = e;
  }

  void abrir_cache() {
    proxima_cache = Reloj::now() + std::chrono::seconds(30);
    // la caché se elige por la huella de la tabla; su ancho de fila tiene que ser el del servidor (leer() copia bytes_fila de la caché)
    try {
      if (Cache::abrir(info.huella, cache) && cache.bytes_fila() != info.bytes_fila) cache = Cache{};
    } catch (const std::exception&) { cache = Cache{}; }  // mmap fallido: sin caché (no puede salir una excepción por la interfaz C)
  }

  bool conectar_unix(const std::string& ruta) {
    sockaddr_un a{};
    if (ruta.size() >= sizeof a.sun_path) { error = "ruta de socket demasiado larga (máximo " + std::to_string(sizeof a.sun_path - 1) + "): " + ruta; return false; }
    fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    a.sun_family = AF_UNIX;
    std::snprintf(a.sun_path, sizeof a.sun_path, "%s", ruta.c_str());
    if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
      error = "no se puede conectar con " + ruta + ": " + std::strerror(errno);
      cerrar();
      return false;
    }
    return true;
  }

  bool conectar_uno() {
    cerrar();
    if (host.starts_with("unix:")) {
      if (!conectar_unix(host.substr(5))) return false;
      return tras_conectar();
    }
    addrinfo pista{}, *res = nullptr;
    pista.ai_socktype = SOCK_STREAM;
    if (int r = ::getaddrinfo(host.c_str(), puerto.c_str(), &pista, &res); r != 0) { error = std::string("getaddrinfo: ") + ::gai_strerror(r); return false; }
    for (auto* a = res; a; a = a->ai_next) {
      fd = ::socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK, a->ai_protocol);
      if (fd < 0) continue;
      // conexión con espera acotada: un servidor inalcanzable no debe parar el paso más de conexion_ms
      int r = ::connect(fd, a->ai_addr, a->ai_addrlen);
      if (r != 0 && errno == EINPROGRESS) {
        pollfd pf{fd, POLLOUT, 0};
        int e = 0;
        socklen_t l = sizeof e;
        if (::poll(&pf, 1, conexion_ms) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &l) == 0 && e == 0) r = 0;
        else errno = e ? e : ETIMEDOUT;
      }
      if (r == 0) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) & ~O_NONBLOCK); break; }
      int guarda = errno;
      cerrar();
      errno = guarda;
    }
    ::freeaddrinfo(res);
    if (fd < 0) { error = "no se puede conectar con " + host + ":" + puerto + ": " + std::strerror(errno); return false; }
    int uno = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
    return tras_conectar();
  }

  bool tras_conectar() {
    const int ms = espera();
    timeval t{ms / 1000, (ms % 1000) * 1000};  // un servidor colgado no debe parar el motor más de la espera
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
    Cabecera c{MAGIA, VERSION, OP_INFO, 0, OK};
    Cabecera r{};
    if (!enviar(&c, sizeof c) || !recibir(&r, sizeof r) || !cabecera_valida(r)) return false;
    if (r.estado != OK) { error = "el servidor rechaza INFO (estado " + std::to_string(r.estado) + ")"; cerrar(); return false; }
    Info nueva{};
    if (!recibir(&nueva, sizeof nueva)) return false;
    // los ids viajan como uint32 y bytes_fila dimensiona los búferes: un INFO absurdo se rechaza en vez de reservar memoria con él
    if (nueva.bytes_fila == 0 || nueva.bytes_fila > (1u << 20) || nueva.filas > (uint64_t{1} << 32)) {
      error = "INFO no válido (" + std::to_string(nueva.filas) + " filas de " + std::to_string(nueva.bytes_fila) + " B)";
      cerrar();
      return false;
    }
    if (huella0 && nueva.huella != huella0) {
      error = "el servidor sirve ahora otra tabla (huella distinta): no se usa";
      cerrar();
      return false;
    }
    info = nueva;
    huella0 = info.huella;
    return true;
  }

  bool cabecera_valida(const Cabecera& r) {
    if (r.magia == MAGIA && r.version == VERSION) return true;
    error = "respuesta que no es de service-ple (magia o versión distintas)";
    cerrar();
    return false;
  }

  // el paso sigue con ceros: avisos al caer y cada minuto
  void caida(uint64_t filas) {
    auto ahora = Reloj::now();
    ++pasos_ceros;
    filas_ceros += filas;
    if (!caido) {
      caido = true;
      caido_desde = ultimo_aviso = ahora;
      aviso("servidor " + host + (puerto.empty() ? "" : ":" + puerto) + " sin respuesta (" + error + "); se sigue con ceros en la PLE y se reintenta cada 5 s");
    } else if (ahora - ultimo_aviso > std::chrono::seconds(60)) {
      ultimo_aviso = ahora;
      aviso("sigue sin servidor desde hace " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(ahora - caido_desde).count()) +
            " s: " + std::to_string(pasos_ceros) + " pasos con ceros (" + error + ")");
    }
  }
  void recuperado() {
    if (!caido) return;
    caido = false;
    aviso("servidor recuperado tras " + std::to_string(std::chrono::duration_cast<std::chrono::seconds>(Reloj::now() - caido_desde).count()) +
          " s; " + std::to_string(pasos_ceros) + " pasos y " + std::to_string(filas_ceros) + " filas fueron con ceros");
    pasos_ceros = filas_ceros = 0;
  }

  bool enviar(const void* b, size_t n) {
    auto* p = static_cast<const uint8_t*>(b);
    // SO_SNDTIMEO vale para cada send: un servidor que lee muy despacio alargaría el envío sin fin. Tope del total: 2 × espera()
    const auto limite = Reloj::now() + std::chrono::milliseconds(2 * espera());
    while (n) {
      ssize_t r = Reloj::now() > limite ? (errno = EAGAIN, -1) : ::send(fd, p, n, MSG_NOSIGNAL);
      if (r <= 0) {
        agotado = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);  // como en recibir: un servidor colgado no se reintenta en el acto
        error = std::string("envío: ") + std::strerror(errno);
        cerrar();
        return false;
      }
      p += r; n -= static_cast<size_t>(r);
    }
    return true;
  }
  bool recibir(void* b, size_t n) {
    auto* p = static_cast<uint8_t*>(b);
    // SO_RCVTIMEO vale para cada recv: un servidor que manda un byte de vez en cuando lo alargaría sin fin. Tope del total: 2 × espera()
    const auto limite = Reloj::now() + std::chrono::milliseconds(2 * espera());
    while (n) {
      ssize_t r = Reloj::now() > limite ? (errno = EAGAIN, -1) : ::recv(fd, p, n, 0);
      if (r <= 0) {
        agotado = r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        error = r == 0 ? "el servidor cerró la conexión" : agotado ? "sin respuesta en " + std::to_string(espera()) + " ms" : std::string("recepción: ") + std::strerror(errno);
        cerrar();
        return false;
      }
      p += r; n -= static_cast<size_t>(r);
    }
    return true;
  }

  // ids → filas (en el mismo orden)
  bool pedir(const std::vector<uint32_t>& q, std::vector<uint8_t>& out) {
    Cabecera c{MAGIA, VERSION, OP_FILAS, static_cast<uint32_t>(q.size()), OK};
    Cabecera r{};
    if (!enviar(&c, sizeof c) || !enviar(q.data(), q.size() * sizeof(uint32_t)) || !recibir(&r, sizeof r) || !cabecera_valida(r)) return false;
    if (r.estado == ERR_SIN_SERVIDOR) { error = "el agente local no tiene servidor ni disco para estas filas"; return false; }  // conexión sana
    if (r.estado != OK || r.n != q.size()) { error = "el servidor rechaza la petición (estado " + std::to_string(r.estado) + ")"; cerrar(); return false; }
    out.resize(q.size() * info.bytes_fila);
    return recibir(out.data(), out.size());
  }
};

void copiar_error(const std::string& e, char* buf, int n) {
  if (buf && n > 0) std::snprintf(buf, static_cast<size_t>(n), "%s", e.c_str());
}

// Encolado en un stream de CUDA (nodo de host): la petición se hace cuando la GPU llega a ese punto de la cola, también dentro de un
// grafo CUDA capturado. Se usa la API del driver (libcuda.so.1, cargada al vuelo) para no depender de la versión del runtime de CUDA.
using CuHostFn = void (*)(void*);
using CuLaunchHostFunc = int (*)(void* stream, CuHostFn fn, void* datos);

struct Encargo {
  Conexion* c;
  const int64_t* ids;
  uint64_t n;
  int64_t lo, hi;
  uint8_t* out;
  bool persistente;  // capturado en un grafo: se reutiliza en cada repetición, no se libera
};

std::mutex mx_error;
std::string ultimo_error;
std::atomic<uint64_t> errores{0};

int sple_filas_impl(Conexion* c, const int64_t* ids, uint64_t n, int64_t lo, int64_t hi, uint8_t* out, std::string& error);

// ninguna excepción (bad_alloc al crecer un búfer, system_error de un mutex) puede salir por la interfaz C ni desde el nodo de host de CUDA:
// las dos acaban en std::terminate y tiran el motor entero. Se escriben ceros y se devuelve el error.
int sple_filas_seguro(Conexion* c, const int64_t* ids, uint64_t n, int64_t lo, int64_t hi, uint8_t* out, std::string& error) noexcept {
  try {
    return sple_filas_impl(c, ids, n, lo, hi, out, error);
  } catch (const std::exception& ex) {
    try { error = std::string("error interno: ") + ex.what(); } catch (...) {}
  } catch (...) {
    try { error = "error interno"; } catch (...) {}
  }
  if (const uint32_t bf = c->info.bytes_fila; out && n) std::memset(out, 0, n * bf);
  return -1;
}

void ejecutar_encargo(void* p) noexcept {
  auto* e = static_cast<Encargo*>(p);
  std::string error;
  if (sple_filas_seguro(e->c, e->ids, e->n, e->lo, e->hi, e->out, error) != 0) {
    // no se puede lanzar una excepción desde aquí: ceros (ya escritos) y error pendiente para el motor
    ++errores;
    try {
      std::lock_guard g(mx_error);
      ultimo_error = error;
    } catch (...) {}
    std::fprintf(stderr, "service-ple: %s\n", error.c_str());
  }
  if (!e->persistente) delete e;
}

CuLaunchHostFunc cu_launch_host_func() {
  static CuLaunchHostFunc f = [] {
    void* lib = ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_NOLOAD);
    if (!lib) lib = ::dlopen("libcuda.so.1", RTLD_NOW);
    return lib ? reinterpret_cast<CuLaunchHostFunc>(::dlsym(lib, "cuLaunchHostFunc")) : nullptr;
  }();
  return f;
}

}  // namespace

extern "C" {

// "host:puerto" → manejador, o nullptr con el motivo en err
static void* sple_abrir_impl(const char* direccion, char* err, int errlen);
void* sple_abrir(const char* direccion, char* err, int errlen) {
  try {
    return sple_abrir_impl(direccion, err, errlen);
  } catch (...) {  // sin memoria: un error, no std::terminate a través de la interfaz C (literal corto: no reserva memoria)
    if (err && errlen > 0) std::snprintf(err, static_cast<size_t>(errlen), "%s", "error interno (sin memoria)");
    return nullptr;
  }
}

static void* sple_abrir_impl(const char* direccion, char* err, int errlen) {
  // "destino[;destino...]": cada destino es "unix:/ruta" (agente local) o "host:puerto" (servidor); se usan en ese orden
  std::string d = direccion ? direccion : "";
  std::unique_ptr<Conexion> propia(new Conexion);  // se libera sola si algo lanza a medio camino
  auto* c = propia.get();
  for (size_t i = 0; i <= d.size();) {
    size_t j = d.find(';', i);
    if (j == std::string::npos) j = d.size();
    if (j > i) c->destinos.push_back(d.substr(i, j - i));
    i = j + 1;
  }
  if (c->destinos.empty()) { copiar_error("dirección vacía", err, errlen); return nullptr; }
  if (!c->conectar()) { copiar_error(c->error, err, errlen); return nullptr; }
  return propia.release();
}

// No cerrar mientras quede trabajo encolado con sple_encolar ni grafos CUDA capturados que lo usen (guardan el manejador).
void sple_cerrar(void* h) { delete static_cast<Conexion*>(h); }

int sple_info(void* h, uint64_t* filas, uint32_t* bytes_fila, uint16_t* escala_bf16, uint64_t* huella) {
  auto* c = static_cast<Conexion*>(h);
  std::lock_guard g(c->mx);  // una reconexión en otro hilo puede estar cambiando info
  *filas = c->info.filas;
  *bytes_fila = c->info.bytes_fila;
  *escala_bf16 = c->info.escala_bf16;
  *huella = c->info.huella;
  return 0;
}

// ids[n] (int64, ids globales de fila) → out[n × bytes_fila]; ceros fuera de [lo, hi) o de la tabla.
// Devuelve 0 si va bien; si no, -1 con el motivo en err. Reintenta una vez reconectando.
int sple_filas(void* h, const int64_t* ids, uint64_t n, int64_t lo, int64_t hi, uint8_t* out, char* err, int errlen) {
  std::string error;
  int r = sple_filas_seguro(static_cast<Conexion*>(h), ids, n, lo, hi, out, error);
  if (r) copiar_error(error, err, errlen);
  return r;
}

// Igual que sple_filas, pero encolado en `stream` (cudaStream_t / CUstream): ids y out deben ser memoria fijada que siga viva hasta que
// la GPU llegue a ese punto. persistente=1 si el stream está capturando un grafo (el encargo se repite en cada ejecución del grafo).
// Los errores no se pueden devolver desde la GPU: se escriben ceros y queda pendiente para sple_errores.
int sple_encolar(void* h, void* stream, const int64_t* ids, uint64_t n, int64_t lo, int64_t hi, uint8_t* out, int persistente, char* err, int errlen) {
  auto f = cu_launch_host_func();
  if (!f) { copiar_error("no se encuentra cuLaunchHostFunc en libcuda.so.1", err, errlen); return -1; }
  auto* e = new (std::nothrow) Encargo{static_cast<Conexion*>(h), ids, n, lo, hi, out, persistente != 0};
  if (!e) { copiar_error("sin memoria para encolar", err, errlen); return -1; }
  if (int r = f(stream, ejecutar_encargo, e); r != 0) {
    delete e;
    copiar_error("cuLaunchHostFunc devolvió " + std::to_string(r), err, errlen);
    return -1;
  }
  return 0;
}

// Número de errores en encargos encolados desde el principio; el último, en err
uint64_t sple_errores(char* err, int errlen) {
  std::lock_guard g(mx_error);
  copiar_error(ultimo_error, err, errlen);
  return errores;
}

}  // extern "C"

namespace {

int sple_filas_impl(Conexion* c, const int64_t* ids, uint64_t n, int64_t lo, int64_t hi, uint8_t* out, std::string& error) {
  std::lock_guard g(c->mx);
  const uint32_t bf = c->info.bytes_fila;
  hi = std::min<int64_t>(hi, static_cast<int64_t>(c->info.filas));
  c->ids.clear();
  for (uint64_t i = 0; i < n; ++i)
    if (ids[i] >= lo && ids[i] < hi) c->ids.push_back(static_cast<uint32_t>(ids[i]));
  std::ranges::sort(c->ids);
  c->ids.erase(std::unique(c->ids.begin(), c->ids.end()), c->ids.end());
  if (c->usado > 0 && c->fd >= 0 && Reloj::now() >= c->proximo_preferido) c->volver_al_preferido();
  if (Reloj::now() >= c->proxima_cache) {  // cada 30 s: aparece la caché del agente, o el agente la ha recreado con otro tamaño
    if (c->cache && !c->cache.vigente()) c->cache = Cache{};
    if (!c->cache) c->abrir_cache(); else c->proxima_cache = Reloj::now() + std::chrono::seconds(30);
  }
  if (!c->ids.empty()) {
    // 1) caché compartida de la máquina (fase 2), sin salir del proceso
    c->filas.resize(c->ids.size() * bf);
    c->faltan.clear();
    c->pos_faltan.clear();
    for (size_t k = 0; k < c->ids.size(); ++k)
      if (!(c->cache && c->cache.leer(c->ids[k], c->filas.data() + k * bf))) { c->faltan.push_back(c->ids[k]); c->pos_faltan.push_back(k); }
    if (c->cache) {
      c->cache.cabecera().aciertos.fetch_add(c->ids.size() - c->faltan.size(), std::memory_order_relaxed);
      c->cache.cabecera().fallos.fetch_add(c->faltan.size(), std::memory_order_relaxed);
    }
    // 2) lo que falta, al agente o al servidor
    if (!c->faltan.empty()) {
      c->agotado = false;
      bool ok = c->fd >= 0 && c->pedir(c->faltan, c->recibidas);
      if (!ok && c->agotado) c->proximo_intento = Reloj::now() + std::chrono::seconds(5);
      // reconexión: enseguida si la conexión se acaba de romper (p. ej. el servidor se reinició); si está caído, cada 5 s
      // (si la conexión sigue abierta, el fallo es del agente sin origen: no se reconecta, el paso va con ceros)
      if (!ok && c->fd < 0 && Reloj::now() >= c->proximo_intento) {
        ok = c->conectar() && c->pedir(c->faltan, c->recibidas);
        if (!ok) c->proximo_intento = Reloj::now() + std::chrono::seconds(5);
      }
      if (!ok) {
        error = c->error;
        if (!c->degradar) { std::memset(out, 0, n * bf); return -1; }
        for (size_t f = 0; f < c->faltan.size(); ++f) std::memset(c->filas.data() + c->pos_faltan[f] * bf, 0, bf);  // las de la caché valen
        c->caida(c->faltan.size());
      } else {
        for (size_t f = 0; f < c->faltan.size(); ++f) std::memcpy(c->filas.data() + c->pos_faltan[f] * bf, c->recibidas.data() + f * bf, bf);
        c->recuperado();
      }
    }
  }
  // SOLO PARA PRUEBAS (medida de la fase 4): SERVICE_PLE_PERDIDAS=0.05 trata como perdidas (ceros) un 5 % fijo de las filas,
  // elegidas por un hash del id (siempre las mismas, como una caché que nunca las tiene)
  static const double perdidas = [] { const char* e = std::getenv("SERVICE_PLE_PERDIDAS"); return e ? std::atof(e) : 0.0; }();
  // umbral sobre 2^64: con perdidas >= 1 se pierden todas (convertir 1,0 × 2^64 a uint64 no está definido)
  const bool todas = perdidas * 18446744073709551616.0 >= 18446744073709551616.0;
  const uint64_t umbral = todas ? 0 : static_cast<uint64_t>(perdidas * 18446744073709551616.0);
  // SERVICE_PLE_RELLENO=anterior: una fila perdida se sustituye por la de la misma cabeza del token anterior (posición i − 16, y así hacia atrás
  // mientras también falte). Con TP la escribe la GPU dueña de esa otra fila, para que la suma entre GPU siga siendo exacta.
  static const bool anterior = [] { const char* e = std::getenv("SERVICE_PLE_RELLENO"); return e && std::string(e) == "anterior"; }();
  constexpr uint64_t CABEZAS = 16;
  auto perdida = [&](int64_t id) {
    return perdidas > 0 && id >= 0 && (todas || (static_cast<uint64_t>(id) * 0x9E3779B97F4A7C15ull ^ 0xD6E8FEB86659FD93ull) * 0xBF58476D1CE4E5B9ull < umbral);
  };
  auto copiar = [&](uint8_t* q, int64_t id) {
    if (id >= lo && id < hi) {
      auto k = std::ranges::lower_bound(c->ids, static_cast<uint32_t>(id)) - c->ids.begin();
      std::memcpy(q, c->filas.data() + static_cast<size_t>(k) * bf, bf);
    } else {
      std::memset(q, 0, bf);
    }
  };
  for (uint64_t i = 0; i < n; ++i) {
    uint8_t* q = out + i * bf;
    if (!perdida(ids[i])) { copiar(q, ids[i]); continue; }
    if (!anterior) { std::memset(q, 0, bf); continue; }
    int64_t j = static_cast<int64_t>(i) - static_cast<int64_t>(CABEZAS);
    while (j >= 0 && perdida(ids[j])) j -= CABEZAS;
    if (j >= 0) copiar(q, ids[j]); else std::memset(q, 0, bf);
  }
  return 0;
}

}  // namespace
