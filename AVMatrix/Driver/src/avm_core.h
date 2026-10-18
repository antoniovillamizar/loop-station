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

typedef struct {
    float               ring[AVM_NUM_SOURCES][AVM_RING_FRAMES];
    _Atomic int64_t     written_end[AVM_SRC_GROUP_COUNT];  // primer frame aún no escrito
    _Atomic int64_t     valid_begin[AVM_SRC_GROUP_COUNT];  // datos anteriores no son válidos
    _Atomic uint32_t    matrix_seq;                         // seqlock: impar = escribiendo
    avm_matrix          matrix;
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
// Preset: Smaart salida N -> LISTENTO entrada N; LISTENTO salida N -> Smaart entrada N.
void avm_matrix_default(avm_matrix* m);
// Devuelve true si la matriz no contiene cruces prohibidos ni valores distintos de 0/1.
bool avm_matrix_is_valid(const avm_matrix* m);

// --- Motor -----------------------------------------------------------------
void avm_engine_init(avm_engine* e);
// Vacía los buffers y la historia de escritura. No usar mientras haya IO activo.
void avm_engine_reset(avm_engine* e);
void avm_engine_set_matrix(avm_engine* e, const avm_matrix* m);
void avm_engine_get_matrix(const avm_engine* e, avm_matrix* out);

// Escribe audio intercalado de una aplicación en las fuentes de un grupo.
// sample_time es la posición en la línea de tiempo común del driver.
void avm_engine_write(avm_engine* e, avm_src_group g, int64_t sample_time,
                      uint32_t frames, const void* interleaved, avm_format fmt,
                      uint32_t channels);

// Produce el audio intercalado de un grupo de destinos según la matriz.
// Las fuentes sin datos válidos para ese instante aportan silencio.
void avm_engine_read(avm_engine* e, avm_dst_group g, int64_t sample_time,
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
