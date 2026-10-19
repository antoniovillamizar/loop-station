// AV Matrix — núcleo de transporte y enrutamiento (C puro, sin CoreAudio).
//
// Este archivo no depende de macOS, para poder probarlo con pruebas unitarias
// en cualquier sistema. El driver HAL (AVMatrixDriver.c) lo usa en el hilo de IO.
//
// Modelo:
//   Fuentes (18):   salidas de Smaart 1-8, salidas de LISTENTO 1-8, Música L/R.
//   Destinos (16):  entradas de Smaart 1-8, entradas de LISTENTO 1-8.
//   Cada cruce de la matriz es 0 (abierto) o 1 (conectado a ganancia unidad).
//   No hay ganancia variable, ecualización, efectos ni delay ajustable.

#ifndef AVM_CORE_H
#define AVM_CORE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tamaño del buffer circular de cada fuente, en frames. Potencia de 2.
#define AVM_RING_FRAMES        65536
// Ventana de validez de los datos de una fuente. Es menor que el buffer para
// que el lector nunca lea posiciones que el escritor está reemplazando.
#define AVM_VALID_WINDOW       (AVM_RING_FRAMES / 2)

#define AVM_CHANNELS_PER_PORT  8
#define AVM_MUSIC_CHANNELS     2

// Grupos de fuentes (lo que las aplicaciones envían AL driver).
typedef enum {
    AVM_SRC_GROUP_SMAART  = 0,  // salidas del dispositivo "Smaart"
    AVM_SRC_GROUP_LINK    = 1,  // salidas del dispositivo "LISTENTO" (retorno de Colombia)
    AVM_SRC_GROUP_MUSIC   = 2,  // salida estéreo del dispositivo "Música"
    AVM_SRC_GROUP_COUNT   = 3
} avm_src_group;

// Grupos de destinos (lo que las aplicaciones reciben DEL driver).
typedef enum {
    AVM_DST_GROUP_SMAART  = 0,  // entradas del dispositivo "Smaart"
    AVM_DST_GROUP_LINK    = 1,  // entradas del dispositivo "LISTENTO" (envío a Colombia)
    AVM_DST_GROUP_COUNT   = 2
} avm_dst_group;

#define AVM_SRC_SMAART_BASE    0
#define AVM_SRC_LINK_BASE      8
#define AVM_SRC_MUSIC_BASE     16
#define AVM_NUM_SOURCES        18

#define AVM_DST_SMAART_BASE    0
#define AVM_DST_LINK_BASE      8
#define AVM_NUM_DESTS          16

// Entrada 8 de Smaart: REFERENCIA LOCAL en el preset de medición.
// Recibe solo Smaart Salida 1 (el generador), nunca el retorno 8 de LISTENTO.
#define AVM_DST_LOCAL_REFERENCE (AVM_DST_SMAART_BASE + 7)
#define AVM_SRC_GENERATOR       (AVM_SRC_SMAART_BASE + 0)

// Formatos de muestra físicos que admite cada stream.
typedef enum {
    AVM_FMT_FLOAT32 = 0,  // 32 bits coma flotante (formato interno, sin conversión)
    AVM_FMT_SINT32  = 1,  // 32 bits entero con signo
    AVM_FMT_SINT24  = 2,  // 24 bits entero con signo, empaquetado (3 bytes)
    AVM_FMT_SINT16  = 3,  // 16 bits entero con signo
    AVM_FMT_COUNT   = 4
} avm_format;

typedef struct {
    uint8_t cell[AVM_NUM_DESTS][AVM_NUM_SOURCES];  // 1 = conectado
} avm_matrix;

// Motivos por los que un cruce puede estar prohibido.
typedef enum {
    AVM_ROUTE_OK = 0,
    AVM_ROUTE_FORBIDDEN_FEEDBACK = 1,  // retorno de LISTENTO -> envío de LISTENTO
    AVM_ROUTE_OUT_OF_RANGE = 2
} avm_route_status;

// Publicación de la matriz sin bloqueos para el hilo de audio:
// el escritor (serializado por el llamador) escribe una ranura que no es la
// publicada y luego publica su índice. Cada ranura lleva su propio contador
// (seqlock con datos atómicos, conforme al modelo de memoria de C11).
// El lector hace como máximo AVM_MATRIX_READ_ATTEMPTS intentos y nunca espera
// al escritor; si no logra una copia coherente, usa la última copia válida.
#define AVM_MATRIX_SLOTS          3
#define AVM_MATRIX_WORDS          ((sizeof(avm_matrix) + 7) / 8)
#define AVM_MATRIX_READ_ATTEMPTS  4

typedef struct {
    _Atomic uint32_t    seq;                         // impar = escritura en curso
    _Atomic uint64_t    words[AVM_MATRIX_WORDS];
} avm_matrix_slot;

typedef struct {
    // Las muestras son atómicas con orden relajado: en arm64 y x86_64 compilan
    // a cargas y almacenamientos normales, y evitan carreras de datos (UB en C11).
    _Atomic float       ring[AVM_NUM_SOURCES][AVM_RING_FRAMES];
    _Atomic int64_t     written_end[AVM_SRC_GROUP_COUNT];  // primer frame aún no escrito
    _Atomic int64_t     valid_begin[AVM_SRC_GROUP_COUNT];  // datos anteriores no son válidos
    // Hasta dónde llega la escritura en curso (se publica ANTES de escribir).
    // El lector lo consulta después de leer: si el escritor dio una vuelta
    // completa al buffer sobre lo leído, descarta el bloque (silencio).
    _Atomic int64_t     writing_end[AVM_SRC_GROUP_COUNT];
    _Atomic uint64_t    overrun_reads;                      // diagnóstico: bloques descartados

    avm_matrix_slot     matrix_slots[AVM_MATRIX_SLOTS];
    _Atomic uint32_t    matrix_published;                  // índice de la ranura vigente
    // Última matriz coherente vista por cada grupo de destinos. Solo la usa el
    // hilo de IO del dispositivo correspondiente (un lector por grupo).
    avm_matrix          reader_cache[AVM_DST_GROUP_COUNT];
    _Atomic uint64_t    stale_matrix_reads;                // diagnóstico: lecturas con copia anterior
} avm_engine;

// --- Descripción de grupos -------------------------------------------------
int  avm_src_group_base(avm_src_group g);
int  avm_src_group_channels(avm_src_group g);
int  avm_dst_group_base(avm_dst_group g);
int  avm_dst_group_channels(avm_dst_group g);
avm_src_group avm_src_group_of(int src);

// --- Reglas de la matriz ---------------------------------------------------
avm_route_status avm_route_allowed(int dst, int src);
void avm_matrix_clear(avm_matrix* m);
// Preset de medición (predeterminado del driver):
//   Smaart Salida N   -> LISTENTO Envío N      (N = 1..8, hacia Colombia)
//   LISTENTO Retorno N -> Smaart Entrada N     (N = 1..7, retornos medidos)
//   Smaart Salida 1   -> Smaart Entrada 8      (REFERENCIA LOCAL, sin el retorno 8)
void avm_matrix_default(avm_matrix* m);
// Preset directo 8x8: Smaart Salida N -> LISTENTO Envío N y LISTENTO Retorno N ->
// Smaart Entrada N para N = 1..8 (sin referencia local).
void avm_matrix_direct(avm_matrix* m);
// Devuelve true si la matriz no contiene cruces prohibidos ni valores distintos de 0/1.
bool avm_matrix_is_valid(const avm_matrix* m);

// --- Motor -----------------------------------------------------------------
void avm_engine_init(avm_engine* e);
// Vacía los buffers y la historia de escritura. No usar mientras haya IO activo.
void avm_engine_reset(avm_engine* e);
// Publica una matriz. Los escritores deben estar serializados por el llamador.
// Nunca bloquea a los lectores.
void avm_engine_set_matrix(avm_engine* e, const avm_matrix* m);
// Lectura acotada y sin espera. Devuelve false (y no modifica *out) si tras
// AVM_MATRIX_READ_ATTEMPTS intentos no obtuvo una copia coherente.
bool avm_engine_try_get_matrix(avm_engine* e, avm_matrix* out);

// Escribe audio intercalado de una aplicación en las fuentes de un grupo.
// sample_time es la posición en la línea de tiempo común del driver.
void avm_engine_write(avm_engine* e, avm_src_group g, int64_t sample_time,
                      uint32_t frames, const void* interleaved, avm_format fmt,
                      uint32_t channels);

// Produce el audio intercalado de un grupo de destinos según la matriz.
// Las fuentes sin datos válidos para ese instante aportan silencio.
// Devuelve false si el bloque se descartó (silencio) porque el escritor
// sobrescribió los datos durante la lectura (lector retrasado > una vuelta del buffer).
bool avm_engine_read(avm_engine* e, avm_dst_group g, int64_t sample_time,
                     uint32_t frames, void* interleaved, avm_format fmt,
                     uint32_t channels);

// --- Conversión de formatos --------------------------------------------------
uint32_t avm_format_bytes_per_sample(avm_format fmt);
uint32_t avm_format_bits(avm_format fmt);
float    avm_sample_to_float(const void* src, avm_format fmt);
void     avm_float_to_sample(float v, void* dst, avm_format fmt);

#ifdef __cplusplus
}
#endif

#endif
