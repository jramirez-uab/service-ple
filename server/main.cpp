// service-ple: servidor de la tabla PLE (fase 1). Carga la tabla entera en RAM (páginas enormes si las hay) y devuelve filas por TCP.
// Uso: service-ple-server --fichero ple-fp8.safetensors [--puerto 18460] [--hilos-carga 8] [--mlock]
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <vector>

#include "formato.hpp"
#include "protocolo.hpp"
#include "tabla.hpp"

#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << 26)
#endif

namespace {

using namespace sple;
using Reloj = std::chrono::steady_clock;

template <class... A>
void log(const char* f, A&&... a) {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  ::localtime_r(&t, &tm);
  char hora[32];
  std::strftime(hora, sizeof hora, "%F %T", &tm);
  std::fputs((std::string(hora) + " " + sple::formato(f, std::forward<A>(a)...) + "\n").c_str(), stdout);
  std::fflush(stdout);
}

struct Memoria {  // la tabla en RAM
  uint8_t* p = nullptr;
  size_t tam = 0;
  const char* tipo = "";
  explicit Memoria(size_t n) {
    constexpr size_t GiB = 1ull << 30;
    size_t redondeo = (n + GiB - 1) / GiB * GiB;
    void* q = ::mmap(nullptr, redondeo, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_HUGE_1GB, -1, 0);
    if (q != MAP_FAILED) { p = static_cast<uint8_t*>(q); tam = redondeo; tipo = "páginas de 1 GiB"; return; }
    q = ::mmap(nullptr, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (q == MAP_FAILED) throw std::runtime_error(std::string("mmap: ") + std::strerror(errno));
    ::madvise(q, n, MADV_HUGEPAGE);
    p = static_cast<uint8_t*>(q); tam = n; tipo = "páginas normales con THP (madvise)";
  }
  ~Memoria() { if (p) ::munmap(p, tam); }
};

void cargar(const std::string& fichero, const DescTabla& d, uint8_t* dst, int hilos) {
  int fd = ::open(fichero.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) throw std::runtime_error("no se puede abrir " + fichero);
  std::atomic<size_t> siguiente{0};
  std::atomic<bool> fallo{false};
  std::vector<std::jthread> h;
  for (int k = 0; k < hilos; ++k)
    h.emplace_back([&] {
      for (size_t i; (i = siguiente++) < d.trozos.size() && !fallo;) {
        const auto& t = d.trozos[i];
        uint8_t* q = dst + static_cast<size_t>(t.numero) * d.filas_por_trozo * d.bytes_fila;
        size_t falta = t.filas * d.bytes_fila;
        off_t off = static_cast<off_t>(t.desplazamiento);
        while (falta) {
          ssize_t r = ::pread(fd, q, std::min<size_t>(falta, 64u << 20), off);
          if (r <= 0) { fallo = true; break; }
          q += r; off += r; falta -= static_cast<size_t>(r);
        }
      }
    });
  h.clear();
  ::close(fd);
  if (fallo) throw std::runtime_error("error leyendo " + fichero);
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

struct Estadisticas {
  std::atomic<uint64_t> peticiones{0}, filas{0}, conexiones{0}, activas{0}, errores{0};
};

void atender(int fd, std::string quien, const DescTabla& d, const uint8_t* tabla, Estadisticas& e) {
  ++e.conexiones; ++e.activas;
  log("conexión de {}", quien);
  int uno = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &uno, sizeof uno);
  const Info info{d.filas, d.bytes_fila, d.escala_bf16, 0, d.huella()};
  std::vector<uint32_t> ids;
  std::vector<uint8_t> salida;
  for (Cabecera c; leer_todo(fd, &c, sizeof c);) {
    Cabecera r{MAGIA, VERSION, c.op, 0, OK};
    if (c.magia != MAGIA) r.estado = ERR_MAGIA;
    else if (c.version != VERSION) r.estado = ERR_VERSION;
    else if (c.op == OP_INFO) {
      salida.resize(sizeof r + sizeof info);
      std::memcpy(salida.data(), &r, sizeof r);
      std::memcpy(salida.data() + sizeof r, &info, sizeof info);
      if (!enviar_todo(fd, salida.data(), salida.size())) break;
      continue;
    } else if (c.op == OP_FILAS) {
      if (c.n > MAX_FILAS_PETICION) r.estado = ERR_DEMASIADAS;
      else {
        ids.resize(c.n);
        if (!leer_todo(fd, ids.data(), c.n * sizeof(uint32_t))) break;
        r.n = c.n;
        salida.resize(sizeof r + static_cast<size_t>(c.n) * d.bytes_fila);
        uint8_t* q = salida.data() + sizeof r;
        for (uint32_t id : ids) {
          if (id >= d.filas) { r.estado = ERR_FUERA_DE_RANGO; r.n = 0; break; }
          std::memcpy(q, tabla + static_cast<size_t>(id) * d.bytes_fila, d.bytes_fila);
          q += d.bytes_fila;
        }
        if (r.estado == OK) {
          std::memcpy(salida.data(), &r, sizeof r);
          if (!enviar_todo(fd, salida.data(), salida.size())) break;
          ++e.peticiones; e.filas += c.n;
          continue;
        }
      }
    } else r.estado = ERR_OP;
    ++e.errores;
    log("{}: petición rechazada (estado {})", quien, r.estado);
    enviar_todo(fd, &r, sizeof r);
    break;
  }
  ::close(fd);
  --e.activas;
  log("cierra {}", quien);
}

}  // namespace

int main(int argc, char** argv) {
  std::string fichero;
  int puerto = 18460, hilos = 8;
  bool bloquear = false;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto sig = [&] { if (i + 1 >= argc) { std::fprintf(stderr, "falta el valor de %s\n", a.c_str()); std::exit(2); } return std::string(argv[++i]); };
    if (a == "--fichero") fichero = sig();
    else if (a == "--puerto") puerto = std::stoi(sig());
    else if (a == "--hilos-carga") hilos = std::stoi(sig());
    else if (a == "--mlock") bloquear = true;
    else { std::fprintf(stderr, "uso: %s --fichero ple-fp8.safetensors [--puerto 18460] [--hilos-carga 8] [--mlock]\n", argv[0]); return 2; }
  }
  if (fichero.empty()) { std::fprintf(stderr, "falta --fichero\n"); return 2; }
  try {
    auto t0 = Reloj::now();
    DescTabla d = leer_cabecera(fichero);
    log("tabla {}: {} filas × {} B en {} trozos de {}; escala bf16 0x{:04x}; huella {:016x}",
        fichero, d.filas, d.bytes_fila, d.trozos.size(), d.filas_por_trozo, d.escala_bf16, d.huella());
    size_t bytes = static_cast<size_t>(d.filas_por_trozo) * d.trozos.size() * d.bytes_fila;
    Memoria m(bytes);
    log("memoria: {:.1f} GiB en {}", m.tam / double(1ull << 30), m.tipo);
    cargar(fichero, d, m.p, hilos);
    if (bloquear && ::mlock(m.p, m.tam) != 0) log("aviso: mlock falló: {}", std::strerror(errno));
    double seg = std::chrono::duration<double>(Reloj::now() - t0).count();
    log("cargada en {:.0f} s ({:.2f} GB/s)", seg, bytes / seg / 1e9);

    int s = ::socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int uno = 1, cero = 0;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &uno, sizeof uno);
    ::setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &cero, sizeof cero);
    sockaddr_in6 dir{};
    dir.sin6_family = AF_INET6;
    dir.sin6_port = htons(static_cast<uint16_t>(puerto));
    dir.sin6_addr = in6addr_any;
    if (::bind(s, reinterpret_cast<sockaddr*>(&dir), sizeof dir) != 0 || ::listen(s, 64) != 0)
      throw std::runtime_error(sple::formato("no se puede escuchar en el puerto {}: {}", puerto, std::strerror(errno)));
    log("servicio listo en el puerto {}", puerto);

    Estadisticas e;
    std::jthread informe([&](std::stop_token st) {
      uint64_t p0 = 0, f0 = 0;
      while (!st.stop_requested()) {
        for (int i = 0; i < 600 && !st.stop_requested(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t p = e.peticiones, f = e.filas;
        if (p != p0) log("último minuto: {} peticiones, {} filas ({:.1f} MB); conexiones activas {}, errores {}", p - p0, f - f0, (f - f0) * d.bytes_fila / 1e6, e.activas.load(), e.errores.load());
        p0 = p; f0 = f;
      }
    });
    std::signal(SIGPIPE, SIG_IGN);
    for (;;) {
      sockaddr_in6 cli{};
      socklen_t l = sizeof cli;
      int c = ::accept4(s, reinterpret_cast<sockaddr*>(&cli), &l, SOCK_CLOEXEC);
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
      char txt[INET6_ADDRSTRLEN] = "?";
      ::inet_ntop(AF_INET6, &cli.sin6_addr, txt, sizeof txt);
      std::string quien = sple::formato("{}:{}", txt, ntohs(cli.sin6_port));
      std::thread(atender, c, std::move(quien), std::cref(d), m.p, std::ref(e)).detach();
    }
  } catch (const std::exception& ex) {
    log("ERROR: {}", ex.what());
    return 1;
  }
}
