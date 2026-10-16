// service-ple fase 2: caché de filas en memoria compartida (/dev/shm/service-ple-<huella>), común a todos los motores de una máquina.
// Asociativa por conjuntos (VIAS huecos por conjunto); expulsión CLOCK (bit de uso que ponen los lectores); un escritor (el agente) y
// muchos lectores (los motores) sin cerrojos: cada hueco lleva un contador de versión (seqlock). Una fila nunca cambia de valor (tabla de
// solo lectura), así que no hay problema de coherencia: como mucho, un lector ve el hueco a medio escribir y lo trata como fallo.
#pragma once
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include "formato.hpp"

namespace sple {

inline constexpr uint64_t MAGIA_CACHE = 0x31454843454c5053ull;  // "SPLECHE1"
inline constexpr uint32_t VIAS = 8;
inline constexpr uint32_t MAX_BYTES_FILA = 160;

struct alignas(64) CabeceraCache {
  alignas(8) uint64_t magia;  // se accede con std::atomic_ref: la escribe el agente al final, la leen los motores
  uint64_t huella;      // tabla a la que pertenecen las filas
  uint64_t conjuntos;   // potencia de 2
  uint32_t bytes_fila;
  uint32_t vias;
  std::atomic<uint64_t> aciertos, fallos, insertadas;  // contadores (los suman los clientes y el agente)
};

struct Hueco {
  std::atomic<uint32_t> version;  // impar = escribiéndose
  std::atomic<uint8_t> uso;       // bit de uso para CLOCK
  uint8_t relleno[3];
  alignas(8) uint64_t id;         // id + 1 (0 = vacío); se accede con std::atomic_ref
  uint8_t fila[MAX_BYTES_FILA];
};
static_assert(sizeof(Hueco) == 176);

struct Conjunto {
  Hueco h[VIAS];
  std::atomic<uint32_t> mano;  // manecilla de CLOCK (solo el escritor)
  uint8_t relleno[60];
};

inline std::string ruta_cache(uint64_t huella) { return formato("/dev/shm/service-ple-{:016x}", huella); }

class Cache {
 public:
  // crear (agente): conjuntos = filas / VIAS redondeado a potencia de 2
  static Cache crear(uint64_t huella, uint32_t bytes_fila, uint64_t filas) {
    if (bytes_fila > MAX_BYTES_FILA) throw std::runtime_error("filas demasiado anchas para la caché");
    if (filas > (uint64_t{1} << 36)) throw std::runtime_error("caché demasiado grande");  // y el bucle de abajo no puede desbordar
    uint64_t c = 1;
    while (c * VIAS < filas) c <<= 1;
    std::string ruta = ruta_cache(huella);
    size_t tam = sizeof(CabeceraCache) + c * sizeof(Conjunto);
    // de otro tamaño (otro --filas): fichero nuevo, nunca ftruncate del que tienen mapeado los motores (con uno más pequeño, SIGBUS);
    // los motores siguen con el antiguo, que sigue siendo correcto (filas que no cambian), hasta que ven el nuevo (vigente())
    int fd = ::open(ruta.c_str(), O_RDWR | O_CLOEXEC);
    bool reutilizar = false;
    if (fd >= 0) {
      struct stat st{};
      reutilizar = ::fstat(fd, &st) == 0 && static_cast<size_t>(st.st_size) == tam;
      if (!reutilizar) { ::close(fd); ::unlink(ruta.c_str()); fd = -1; }
    }
    if (fd < 0) {
      fd = ::open(ruta.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0660);
      if (fd < 0) throw std::runtime_error("no se puede crear " + ruta);
      if (::ftruncate(fd, static_cast<off_t>(tam)) != 0) { ::close(fd); throw std::runtime_error("ftruncate " + ruta); }
    }
    Cache k(fd, tam);
    auto* cab = k.cab_;
    if (!reutilizar || std::atomic_ref<uint64_t>(cab->magia).load(std::memory_order_acquire) != MAGIA_CACHE || cab->huella != huella || cab->conjuntos != c || cab->bytes_fila != bytes_fila) {
      std::memset(static_cast<void*>(k.base_), 0, tam);  // nueva (o de otra tabla/tamaño): vacía
      cab->huella = huella; cab->conjuntos = c; cab->bytes_fila = bytes_fila; cab->vias = VIAS;
      std::atomic_ref<uint64_t>(cab->magia).store(MAGIA_CACHE, std::memory_order_release);  // la última: un motor no ve una cabecera a medias
      k.reutilizada_ = false;
    } else {
      // un agente anterior que murió a medio escribir deja huecos con versión impar: los lectores los saltarían siempre y el escritor,
      // al reutilizarlos, pondría la versión en par mientras escribe. Se vacían (aquí no hay otro escritor).
      for (uint64_t i = 0; i < c; ++i)
        for (auto& h : k.conj_[i].h)
          if (uint32_t v = h.version.load(std::memory_order_relaxed); v & 1u) {
            std::atomic_ref<uint64_t>(h.id).store(0, std::memory_order_relaxed);
            h.version.store(v + 1, std::memory_order_release);
          }
      k.reutilizada_ = true;
    }
    k.conjuntos_ = c; k.bf_ = bytes_fila; k.huella_ = huella;
    return k;
  }

  // abrir (motor): nullptr-equivalente si no existe o no es de esta tabla
  static bool abrir(uint64_t huella, Cache& out) {
    std::string ruta = ruta_cache(huella);
    int fd = ::open(ruta.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return false;
    struct stat st{};
    if (::fstat(fd, &st) != 0 || static_cast<size_t>(st.st_size) < sizeof(CabeceraCache)) { ::close(fd); return false; }
    Cache k(fd, static_cast<size_t>(st.st_size));
    // la cabecera viene de un fichero de /dev/shm: se valida todo lo que luego da tamaños o índices
    const auto& cab = *k.cab_;
    const uint64_t c = cab.conjuntos;
    if (std::atomic_ref<uint64_t>(k.cab_->magia).load(std::memory_order_acquire) != MAGIA_CACHE || cab.huella != huella || cab.vias != VIAS || cab.bytes_fila == 0 || cab.bytes_fila > MAX_BYTES_FILA ||
        c == 0 || (c & (c - 1)) != 0 || c > (static_cast<size_t>(st.st_size) - sizeof(CabeceraCache)) / sizeof(Conjunto) ||
        sizeof(CabeceraCache) + c * sizeof(Conjunto) != static_cast<size_t>(st.st_size)) return false;
    // copia propia de la geometría: lo que haya después en la cabecera compartida no puede llevar a este proceso fuera de su mmap
    k.conjuntos_ = c; k.bf_ = cab.bytes_fila; k.huella_ = huella;
    out = std::move(k);
    return true;
  }

  Cache() = default;
  Cache(Cache&& o) noexcept { *this = std::move(o); }
  Cache& operator=(Cache&& o) noexcept {
    if (this != &o) {
      cerrar();
      base_ = o.base_; tam_ = o.tam_; cab_ = o.cab_; conj_ = o.conj_; reutilizada_ = o.reutilizada_;
      conjuntos_ = o.conjuntos_; bf_ = o.bf_; huella_ = o.huella_; ino_ = o.ino_;
      o.base_ = nullptr;
    }
    return *this;
  }
  ~Cache() { cerrar(); }
  explicit operator bool() const { return base_ != nullptr; }
  bool reutilizada() const { return reutilizada_; }
  uint64_t capacidad() const { return conjuntos_ * VIAS; }
  uint32_t bytes_fila() const { return bf_; }
  // el fichero de /dev/shm sigue siendo este (el agente no lo ha sustituido por otro de distinto tamaño)
  bool vigente() const {
    struct stat st{};
    return ::stat(ruta_cache(huella_).c_str(), &st) == 0 && st.st_ino == ino_;
  }
  CabeceraCache& cabecera() { return *cab_; }

  // lector: copia la fila si está; marca el bit de uso
  bool leer(uint64_t id, uint8_t* dst) const {
    Conjunto& s = conj_[conjunto(id)];
    const uint32_t bf = bf_;
    for (auto& h : s.h) {
      uint32_t v1 = h.version.load(std::memory_order_acquire);
      if (v1 & 1u) continue;
      if (id_de(h) != id + 1) continue;
      std::memcpy(dst, h.fila, bf);
      std::atomic_thread_fence(std::memory_order_acquire);
      if (h.version.load(std::memory_order_relaxed) != v1 || id_de(h) != id + 1) return false;  // a medio cambiar: fallo
      h.uso.store(1, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  // escritor (un solo hilo a la vez): inserta o refresca; expulsa con CLOCK
  void escribir(uint64_t id, const uint8_t* src) {
    Conjunto& s = conj_[conjunto(id)];
    for (auto& h : s.h)
      if (id_de(h) == id + 1) { h.uso.store(1, std::memory_order_relaxed); return; }
    Hueco* v = nullptr;
    for (auto& h : s.h) if (id_de(h) == 0) { v = &h; break; }
    while (!v) {
      uint32_t m = s.mano.load(std::memory_order_relaxed);
      Hueco& h = s.h[m % VIAS];
      s.mano.store(m + 1, std::memory_order_relaxed);
      if (h.uso.exchange(0, std::memory_order_relaxed) == 0) v = &h;
    }
    uint32_t ver = v->version.load(std::memory_order_relaxed);
    v->version.store(ver + 1, std::memory_order_relaxed);  // impar: escribiéndose
    std::atomic_thread_fence(std::memory_order_release);
    std::atomic_ref<uint64_t>(v->id).store(id + 1, std::memory_order_relaxed);
    std::memcpy(v->fila, src, bf_);
    v->uso.store(1, std::memory_order_relaxed);
    v->version.store(ver + 2, std::memory_order_release);
    cab_->insertadas.fetch_add(1, std::memory_order_relaxed);
  }

 private:
  Cache(int fd, size_t tam) : tam_(tam) {
    struct stat st{};
    if (::fstat(fd, &st) == 0) ino_ = st.st_ino;
    void* p = ::mmap(nullptr, tam, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) throw std::runtime_error("mmap de la caché");
    base_ = static_cast<uint8_t*>(p);
    cab_ = reinterpret_cast<CabeceraCache*>(base_);
    conj_ = reinterpret_cast<Conjunto*>(base_ + sizeof(CabeceraCache));
  }
  // id lo escribe el agente mientras los motores lo leen: atómico (relajado; el orden lo da la versión)
  static uint64_t id_de(Hueco& h) { return std::atomic_ref<uint64_t>(h.id).load(std::memory_order_relaxed); }
  void cerrar() { if (base_) ::munmap(base_, tam_); base_ = nullptr; }
  uint64_t conjunto(uint64_t id) const { return (id * 0x9E3779B97F4A7C15ull >> 20) & (conjuntos_ - 1); }

  uint8_t* base_ = nullptr;
  size_t tam_ = 0;
  CabeceraCache* cab_ = nullptr;
  Conjunto* conj_ = nullptr;
  bool reutilizada_ = false;
  uint64_t conjuntos_ = 0, huella_ = 0;
  uint32_t bf_ = 0;
  ino_t ino_ = 0;
};

}  // namespace sple
