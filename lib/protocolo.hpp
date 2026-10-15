// service-ple: protocolo entre el servidor de la tabla PLE y sus clientes (versión 1).
// Todo en little-endian (x86-64). Una petición = Cabecera [+ n ids uint32]; una respuesta = Cabecera [+ cuerpo].
//   INFO  (op 1): respuesta Cabecera + Info.
//   FILAS (op 2): petición con n ids globales (ordenados y sin repetir, recomendado); respuesta Cabecera(n) + n × bytes_fila,
//                 en el mismo orden. Si estado != 0 no hay cuerpo y el servidor cierra la conexión.
// Idempotencia: todas las operaciones son de solo lectura sobre una tabla que no cambia; repetir una petición da el mismo resultado y no cambia
// nada en el servidor. Un cliente puede reintentar cualquier petición (por ejemplo, tras reconectar) sin riesgo.
#pragma once
#include <cstdint>

namespace sple {

inline constexpr uint32_t MAGIA = 0x31454c50;  // "PLE1"
inline constexpr uint16_t VERSION = 1;
inline constexpr uint32_t MAX_FILAS_PETICION = 1u << 22;  // 4 M filas (640 MB): de sobra para un trozo de prefill

enum Op : uint16_t { OP_INFO = 1, OP_FILAS = 2 };
enum Estado : uint32_t { OK = 0, ERR_MAGIA = 1, ERR_VERSION = 2, ERR_OP = 3, ERR_DEMASIADAS = 4, ERR_FUERA_DE_RANGO = 5, ERR_SIN_SERVIDOR = 6 };

struct Cabecera {
  uint32_t magia;
  uint16_t version;
  uint16_t op;
  uint32_t n;
  uint32_t estado;
};
static_assert(sizeof(Cabecera) == 16);

struct Info {
  uint64_t filas;        // filas de la tabla
  uint32_t bytes_fila;   // 160 en Flash-Next (fp8 e4m3)
  uint16_t escala_bf16;  // escala global de la tabla, tal cual está en el fichero
  uint16_t reservado;
  uint64_t huella;       // FNV-1a de 64 bits de la cabecera JSON del safetensors: identifica la tabla
};
static_assert(sizeof(Info) == 24);

inline uint64_t fnv1a(const void* datos, uint64_t n) {
  auto* p = static_cast<const unsigned char*>(datos);
  uint64_t h = 0xcbf29ce484222325ull;
  for (uint64_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
  return h;
}

}  // namespace sple
