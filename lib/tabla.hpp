// service-ple: lectura de la tabla PLE desde ple-fp8.safetensors (trozos `...ngram_embedding.shard_N.weight` + `weight_scale`).
// Los trozos están en el fichero en orden alfabético (0, 1, 10, 100...): se colocan por su número, fila global = N × filas_por_trozo + fila.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace sple {

// JSON mínimo: lo justo para la cabecera de un safetensors (objetos, listas, cadenas, enteros).
struct Json {
  std::variant<std::nullptr_t, long long, std::string, std::vector<Json>, std::map<std::string, Json>> v;
  const std::map<std::string, Json>& obj() const { return std::get<std::map<std::string, Json>>(v); }
  const std::vector<Json>& lista() const { return std::get<std::vector<Json>>(v); }
  const std::string& cad() const { return std::get<std::string>(v); }
  long long ent() const { return std::get<long long>(v); }
};
Json parse_json(const std::string& s);

struct Trozo {
  int numero;
  uint64_t filas;
  uint64_t desplazamiento;  // en el fichero, desde el principio
};

struct DescTabla {
  std::string cabecera;  // JSON tal cual (para la huella)
  uint64_t inicio_datos = 0;
  uint32_t bytes_fila = 0;
  uint64_t filas_por_trozo = 0;
  uint64_t filas = 0;
  uint16_t escala_bf16 = 0;
  std::vector<Trozo> trozos;  // ordenados por número
  uint64_t huella() const;
};

DescTabla leer_cabecera(const std::string& fichero);

}  // namespace sple
