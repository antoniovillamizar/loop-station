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
#include <stddef.h>
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

// --- Medidores de nivel ------------------------------------------------------
// Solo observan: leen los mismos valores float que el motor ya calcula y nunca
// modifican, retrasan ni copian las muestras transportadas.
//   * Fuente: valor de cada muestra que la aplicación envía (antes de la matriz;
//     incluye la mezcla que hace macOS si varias apps usan la misma salida).
//   * Destino: valor sumado por la matriz que se entrega a la aplicación
//     (después del enrutamiento y de la suma; antes de recortar a entero).
// Cada medidor tiene UN escritor (el hilo de IO de su dispositivo) y se
// actualiza sin esperas: solo cargas y almacenamientos atómicos.
// El pico se agrupa en ventanas de AVM_METER_EPOCH_NS; la lectura devuelve el
// máximo de la ventana actual y la anterior, sin borrar nada, así varios
// lectores (app, avmctl) no se roban los picos. Si no llegan bloques nuevos
// durante AVM_METER_ACTIVE_NS, el medidor pasa a inactivo y su pico a 0.
#define AVM_METER_EPOCH_NS   50000000ull     // 50 ms
#define AVM_METER_ACTIVE_NS  250000000ull    // 250 ms sin bloques = inactivo
#define AVM_METER_CLIP_HOLD_NS 2000000000ull // el indicador rojo se mantiene 2 s

typedef struct {
    _Atomic uint64_t    epoch;
    _Atomic float       cur_peak;       // pico de la ventana `epoch`
    _Atomic float       prev_peak;      // pico de la ventana anterior
    _Atomic uint64_t    last_update_ns; // 0 = nunca
    _Atomic uint64_t    last_clip_ns;   // 0 = nunca
} avm_meter;

typedef struct {
    float   peak;       // pico lineal (1.0 = 0 dBFS); 0 si inactivo
    bool    active;     // llegaron bloques en los últimos AVM_METER_ACTIVE_NS
    bool    clip;       // saturación en los últimos AVM_METER_CLIP_HOLD_NS
} avm_meter_reading;

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

    avm_meter           src_meters[AVM_NUM_SOURCES];
    avm_meter           dst_meters[AVM_NUM_DESTS];
    // Sesión de los medidores (seqlock): avm_engine_reset lo pone impar mientras
    // los limpia y par al terminar. Un lector que lo ve impar o cambiado durante
    // su lectura informa «inactivo»: nunca mezcla valores de dos sesiones.
    _Atomic uint32_t    meter_session;
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
// Vacía los buffers, la historia de escritura y los medidores (nueva sesión).
// No usar mientras haya IO activo; los lectores de medidores pueden seguir
// leyendo durante el reinicio: verán «inactivo» y nunca picos de la sesión anterior.
void avm_engine_reset(avm_engine* e);
// Publica una matriz. Los escritores deben estar serializados por el llamador.
// Nunca bloquea a los lectores.
void avm_engine_set_matrix(avm_engine* e, const avm_matrix* m);
// Lectura acotada y sin espera. Devuelve false (y no modifica *out) si tras
// AVM_MATRIX_READ_ATTEMPTS intentos no obtuvo una copia coherente.
bool avm_engine_try_get_matrix(avm_engine* e, avm_matrix* out);

// Variantes con medición: now_ns es la hora monotónica en nanosegundos (> 0).
// Hacen exactamente lo mismo que avm_engine_write/avm_engine_read y además
// actualizan los medidores de las fuentes o destinos del grupo.
void avm_engine_write_metered(avm_engine* e, avm_src_group g, int64_t sample_time,
                              uint32_t frames, const void* interleaved, avm_format fmt,
                              uint32_t channels, uint64_t now_ns);
bool avm_engine_read_metered(avm_engine* e, avm_dst_group g, int64_t sample_time,
                             uint32_t frames, void* interleaved, avm_format fmt,
                             uint32_t channels, uint64_t now_ns);

// Lectura de todos los medidores (no modifica nada; segura desde cualquier hilo).
void avm_engine_meter_snapshot(avm_engine* e, uint64_t now_ns,
                               avm_meter_reading src[AVM_NUM_SOURCES],
                               avm_meter_reading dst[AVM_NUM_DESTS]);

// Serialización de los medidores para la propiedad 'metr' del driver.
// Formato: cabecera 16 bytes ('AVM1', n fuentes, n destinos, 0) y 8 bytes por
// medidor (float pico, uint8 activo, uint8 saturación, uint16 0), fuentes y luego destinos.
#define AVM_METERS_BLOB_SIZE (16u + 8u * (AVM_NUM_SOURCES + AVM_NUM_DESTS))
void avm_meters_pack(const avm_meter_reading src[AVM_NUM_SOURCES],
                     const avm_meter_reading dst[AVM_NUM_DESTS], uint8_t out[AVM_METERS_BLOB_SIZE]);
bool avm_meters_unpack(const uint8_t* blob, size_t len,
                       avm_meter_reading src[AVM_NUM_SOURCES], avm_meter_reading dst[AVM_NUM_DESTS]);

// Valor float del código positivo de escala completa de un formato
// (32767/32768, 8388607/8388608; 1,0 para Float32 y SInt32).
float avm_format_clip_level(avm_format fmt);

// Saturación según la conversión real al formato `fmt`: true si, al convertir
// el valor más alto (`hi`) o el más bajo (`lo`) de un bloque con la misma
// función que entrega las muestras (avm_float_to_sample: redondeo al par con
// nearbyint y recorte), resulta el código máximo o el mínimo del formato.
// En Float32: hi >= 1,0 o lo <= -1,0. Se usa igual para fuentes y destinos.
// Ejemplos SInt16: 32766,5/32768 -> 32766 (no satura); -32767,5/32768 -> -32768 (satura).
bool avm_format_is_full_scale(float hi, float lo, avm_format fmt);

// Valor float tras pasar por el formato `fmt` (ida y vuelta, con la misma
// conversión que el driver). Sirve para calcular valores esperados exactos.
float avm_quantize(float v, avm_format fmt);

// Pico lineal -> dBFS (20·log10). Devuelve -INFINITY para 0 (silencio digital).
float avm_peak_to_dbfs(float peak);

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
