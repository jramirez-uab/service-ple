#include "tabla.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

#include "protocolo.hpp"

namespace sple {

namespace {
struct Fd {  // descriptor que se cierra solo
  int fd;
  explicit Fd(int f) : fd(f) {}
  ~Fd() { if (fd >= 0) ::close(fd); }
  Fd(const Fd&) = delete;
  Fd& operator=(const Fd&) = delete;
};

struct Lector {
  const std::string& s;
  size_t i = 0;
  int profundidad = 0;  // la cabecera puede llegar a 64 MB: sin tope, "[[[[…" agotaría la pila
  void esp() { while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i; }
  [[noreturn]] void error(const char* q) { throw std::runtime_error(std::string("JSON: ") + q + " en la posición " + std::to_string(i)); }
  char ver() { esp(); if (i >= s.size()) error("fin inesperado"); return s[i]; }
  void esperar(char c) { if (ver() != c) error("carácter inesperado"); ++i; }
  std::string cadena() {
    esperar('"');
    std::string r;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\') { ++i; if (i >= s.size()) error("escape"); }
      r += s[i++];
    }
    esperar('"');
    return r;
  }
  Json valor() {
    struct Nivel { int& p; explicit Nivel(Lector& l) : p(l.profundidad) { if (++p > 64) l.error("anidamiento excesivo"); } ~Nivel() { --p; } } nivel(*this);
    char c = ver();
    if (c == '{') {
      ++i; std::map<std::string, Json> m;
      if (ver() == '}') { ++i; return {m}; }
      for (;;) {
        std::string k = cadena(); esperar(':'); m.emplace(std::move(k), valor());
        if (ver() == ',') { ++i; continue; }
        esperar('}'); return {m};
      }
    }
    if (c == '[') {
      ++i; std::vector<Json> l;
      if (ver() == ']') { ++i; return {l}; }
      for (;;) {
        l.push_back(valor());
        if (ver() == ',') { ++i; continue; }
        esperar(']'); return {l};
      }
    }
    if (c == '"') return {cadena()};
    if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) {
      size_t f; long long n;
      try { n = std::stoll(s.substr(i, 32), &f); } catch (const std::logic_error&) { error("número no válido"); }
      i += f; return {n};
    }
    if (s.compare(i, 4, "null") == 0) { i += 4; return {nullptr}; }
    error("valor no admitido");
  }
};
}  // namespace

// "...shard_N.weight" → N; si no, -1
int numero_de_trozo(const std::string& k) {
  static const std::string fin = ".weight", ini = ".shard_";
  if (!k.ends_with(fin)) return -1;
  auto p = k.rfind(ini);
  if (p == std::string::npos) return -1;
  auto num = k.substr(p + ini.size(), k.size() - fin.size() - p - ini.size());
  if (num.empty() || num.size() > 6 || !std::ranges::all_of(num, [](char c) { return c >= '0' && c <= '9'; })) return -1;
  return std::stoi(num);
}

Json parse_json(const std::string& s) {
  Lector l{s};
  Json j = l.valor();
  l.esp();  // safetensors rellena la cabecera con espacios; cualquier otra cosa detrás del JSON es un fichero dañado
  if (l.i != s.size()) l.error("datos detrás del JSON");
  return j;
}

uint64_t DescTabla::huella() const { return fnv1a(cabecera.data(), cabecera.size()); }

DescTabla leer_cabecera(const std::string& fichero) {
  Fd f(::open(fichero.c_str(), O_RDONLY | O_CLOEXEC));
  const int fd = f.fd;
  if (fd < 0) throw std::runtime_error("no se puede abrir " + fichero + ": " + std::strerror(errno));
  uint64_t n = 0;
  struct stat st{};
  if (::fstat(fd, &st) != 0) throw std::runtime_error("no se puede leer el tamaño de " + fichero);
  if (::pread(fd, &n, 8, 0) != 8 || n == 0 || n > (64u << 20)) { throw std::runtime_error("cabecera safetensors no válida"); }
  DescTabla d;
  d.cabecera.resize(n);
  if (::pread(fd, d.cabecera.data(), n, 8) != static_cast<ssize_t>(n)) { throw std::runtime_error("cabecera safetensors corta"); }
  d.inicio_datos = 8 + n;
  Json j = parse_json(d.cabecera);
  uint64_t escala_desp = 0;
  bool hay_escala = false;
  const uint64_t datos = static_cast<uint64_t>(st.st_size) > d.inicio_datos ? static_cast<uint64_t>(st.st_size) - d.inicio_datos : 0;
  for (const auto& [k, v] : j.obj()) try {
    if (k == "__metadata__") continue;
    const auto& o = v.obj();
    const auto& off = o.at("data_offsets").lista();
    const auto& forma = o.at("shape").lista();
    int numero = numero_de_trozo(k);
    // desplazamientos del fichero, que puede estar dañado: 0 <= inicio <= fin
    if (off.size() != 2 || off[0].ent() < 0 || off[1].ent() < off[0].ent()) throw std::runtime_error("data_offsets no válidos: " + k);
    if (static_cast<uint64_t>(off[1].ent()) > datos) throw std::runtime_error("datos fuera del fichero (¿truncado?): " + k);
    if (numero >= 0) {
      if (o.at("dtype").cad() != "F8_E4M3" || forma.size() != 2) throw std::runtime_error("trozo con formato inesperado: " + k);
      if (forma[0].ent() <= 0 || forma[1].ent() <= 0 || forma[1].ent() > (1 << 20)) throw std::runtime_error("forma no válida: " + k);
      uint32_t bf = static_cast<uint32_t>(forma[1].ent());
      if (d.bytes_fila && d.bytes_fila != bf) throw std::runtime_error("trozos con anchos distintos");
      d.bytes_fila = bf;
      uint64_t filas = static_cast<uint64_t>(forma[0].ent());
      if (filas > UINT64_MAX / bf || static_cast<uint64_t>(off[1].ent() - off[0].ent()) != filas * bf) throw std::runtime_error("tamaño de trozo incoherente: " + k);
      d.trozos.push_back({numero, filas, d.inicio_datos + static_cast<uint64_t>(off[0].ent())});
    } else if (k.ends_with("weight_scale")) {
      if (o.at("dtype").cad() != "BF16") throw std::runtime_error("escala no BF16");
      if (hay_escala) throw std::runtime_error("más de una escala (weight_scale): " + k);
      escala_desp = d.inicio_datos + static_cast<uint64_t>(off[0].ent());
      hay_escala = true;
    }
  } catch (const std::logic_error& e) {  // falta una clave o tiene otro tipo (std::out_of_range, std::bad_variant_access): decir cuál
    throw std::runtime_error("tensor " + k + " mal descrito en la cabecera (" + e.what() + ")");
  } catch (const std::bad_variant_access&) {
    throw std::runtime_error("tensor " + k + " mal descrito en la cabecera");
  }
  if (d.trozos.empty() || !hay_escala) { throw std::runtime_error("el fichero no parece una tabla PLE (faltan trozos o escala)"); }
  if (::pread(fd, &d.escala_bf16, 2, static_cast<off_t>(escala_desp)) != 2) { throw std::runtime_error("no se puede leer la escala"); }
  std::ranges::sort(d.trozos, {}, &Trozo::numero);
  d.filas_por_trozo = d.trozos.front().filas;
  for (size_t i = 0; i < d.trozos.size(); ++i) {
    const auto& t = d.trozos[i];
    if (t.numero != static_cast<int>(i)) throw std::runtime_error("falta el trozo " + std::to_string(i));
    if (i + 1 < d.trozos.size() && t.filas != d.filas_por_trozo) throw std::runtime_error("trozo intermedio con otro tamaño");
    if (t.filas > d.filas_por_trozo) throw std::runtime_error("último trozo mayor que los demás");
  }
  // el servidor reserva filas_por_trozo × trozos × bytes_fila: sin desbordar
  if (d.filas_por_trozo > UINT64_MAX / d.bytes_fila / d.trozos.size()) throw std::runtime_error("tabla demasiado grande");
  d.filas = d.filas_por_trozo * (d.trozos.size() - 1) + d.trozos.back().filas;
  return d;
}

}  // namespace sple
