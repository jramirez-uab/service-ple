// service-ple: formateo mínimo con la sintaxis de std::format ("{}", "{:016x}", "{:.1f}"...) sobre snprintf, para compilar también con
// GCC 11-12 (Debian 12, RHEL/Rocky 9, Ubuntu 22.04), que no tienen <format>. Solo admite enteros, coma flotante y cadenas.
#pragma once
#include <cstdio>
#include <string>
#include <type_traits>

namespace sple {

namespace detalle {

inline void uno(std::string& out, const std::string& spec, const std::string& s) {
  std::string f = "%" + spec + "s";
  int n = std::snprintf(nullptr, 0, f.c_str(), s.c_str());
  if (n < 0) return;  // error de codificación: nada (no un tamaño de SIZE_MAX)
  std::string b(static_cast<size_t>(n) + 1, '\0');
  std::snprintf(b.data(), b.size(), f.c_str(), s.c_str());
  out.append(b.data(), static_cast<size_t>(n));
}
inline void uno(std::string& out, const std::string& spec, const char* s) { uno(out, spec, std::string(s ? s : "(null)")); }

template <class T>
std::enable_if_t<std::is_arithmetic_v<T>> uno(std::string& out, const std::string& spec, T v) {
  std::string f = "%" + spec;
  char tipo = spec.empty() ? '\0' : spec.back();
  char buf[128];
  if constexpr (std::is_floating_point_v<T>) {
    if (tipo != 'f' && tipo != 'e' && tipo != 'g') f += "g";
    std::snprintf(buf, sizeof buf, f.c_str(), static_cast<double>(v));
  } else if constexpr (std::is_same_v<T, bool>) {
    std::snprintf(buf, sizeof buf, "%s", v ? "true" : "false");
  } else {
    bool hex = tipo == 'x' || tipo == 'X';
    if (hex) f.pop_back();
    if constexpr (std::is_signed_v<T>) {
      f += hex ? std::string("ll") + tipo : "lld";
      std::snprintf(buf, sizeof buf, f.c_str(), static_cast<long long>(v));
    } else {
      f += hex ? std::string("ll") + tipo : "llu";
      std::snprintf(buf, sizeof buf, f.c_str(), static_cast<unsigned long long>(v));
    }
  }
  out += buf;
}

inline void resto(std::string& out, const char* f) {
  for (; *f; ++f) {
    if ((f[0] == '{' && f[1] == '{') || (f[0] == '}' && f[1] == '}')) ++f;
    out += *f;
  }
}

template <class A, class... R>
void resto(std::string& out, const char* f, A&& a, R&&... r) {
  for (; *f; ++f) {
    if ((f[0] == '{' && f[1] == '{') || (f[0] == '}' && f[1] == '}')) { out += *f++; continue; }
    if (*f == '{') {
      const char* fin = f;
      while (*fin && *fin != '}') ++fin;
      std::string spec(f + 1, fin);
      if (!spec.empty() && spec[0] == ':') spec.erase(0, 1);
      if (!spec.empty() && spec[0] == '>') spec.erase(0, 1);
      else if (!spec.empty() && spec[0] == '<') { spec.erase(0, 1); spec.insert(0, "-"); }
      uno(out, spec, std::forward<A>(a));
      return resto(out, *fin ? fin + 1 : fin, std::forward<R>(r)...);
    }
    out += *f;
  }
}

}  // namespace detalle

template <class... A>
std::string formato(const char* f, A&&... a) {
  std::string out;
  detalle::resto(out, f, std::forward<A>(a)...);
  return out;
}

}  // namespace sple
