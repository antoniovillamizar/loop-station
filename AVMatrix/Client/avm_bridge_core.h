// AV Matrix — lógica de los puentes Dante y EVO (C puro, sin CoreAudio).
//
// Un puente une un dispositivo de AV Matrix con un dispositivo externo dentro
// de la app, sin DAW ni Loopback:
//
//   Dante: «AV Matrix Dante (puente)» (2 entradas: Dante L/R de la matriz)
//          -> salidas elegidas de Dante Virtual Soundcard (por defecto 1 y 2).
//   EVO:   entradas elegidas de la EVO (por defecto 1-4)
//          -> «AV Matrix EVO (puente)» (4 salidas: fuentes EVO 1-4 de la matriz).
//
// La parte de CoreAudio (avm_bridge.c) crea un dispositivo agregado privado con
// el dispositivo de AV Matrix primero y el externo después; el reloj es el del
// externo y el lado de AV Matrix lleva compensación de deriva. Así, en cada
// sentido, los canales del agregado quedan:
//   Dante: entradas 0-1 = Dante L/R;     salidas = las de DVS (canal n -> n-1).
//   EVO:   entradas = las de la EVO (canal n -> n-1); salidas 0-3 = EVO 1-4,
//          después las salidas propias de la EVO (siempre en silencio).
//
// Este archivo decide (sin tocar el sistema) si el puente debe funcionar, con
// qué mensaje, y copia el audio en el hilo de IO sin esperas ni memoria nueva.

#ifndef AVM_BRIDGE_CORE_H
#define AVM_BRIDGE_CORE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { AVM_BRIDGE_DANTE = 0, AVM_BRIDGE_EVO = 1 } avm_bridge_kind;

#define AVM_BRIDGE_MAX_CH     4      // canales de AV Matrix por puente (Dante 2, EVO 4)
#define AVM_BRIDGE_MAX_BUFS   64     // buffers (streams) por sentido en el agregado
#define AVM_BRIDGE_FADE_FRAMES 256   // fundido de entrada al arrancar (sin chasquido)

typedef struct {
    avm_bridge_kind kind;
    bool  enabled;                       // el usuario pulsó «Activar»
    char  device_uid[256];               // dispositivo externo (DVS o EVO); "" = ninguno
    int   channel[AVM_BRIDGE_MAX_CH];    // canal externo (desde 1) para cada canal de AV Matrix; 0 = ninguno
} avm_bridge_config;

int  avm_bridge_channels(avm_bridge_kind kind);          // 2 (Dante) o 4 (EVO)
void avm_bridge_config_default(avm_bridge_config* c, avm_bridge_kind kind);

typedef enum {
    AVM_BRIDGE_OFF = 0,          // detenido por el usuario
    AVM_BRIDGE_RUN,              // debe funcionar (o funciona)
    AVM_BRIDGE_WAIT_DEVICE,      // activado, pero el dispositivo externo no está: se reconecta solo
    AVM_BRIDGE_WAIT_AVMATRIX,    // falta el dispositivo puente de AV Matrix (driver)
    AVM_BRIDGE_RATE_MISMATCH,    // frecuencias distintas: no se arranca hasta igualarlas
    AVM_BRIDGE_BAD_CONFIG,       // canales no válidos, dispositivo de AV Matrix o en conflicto
    AVM_BRIDGE_SUSPENDED,        // pausado mientras AV Matrix cambia de frecuencia
    AVM_BRIDGE_ERROR             // CoreAudio no pudo arrancarlo (se indica el motivo)
} avm_bridge_state;

// Lo que se sabe del sistema en este momento.
typedef struct {
    bool     avm_present;            // el dispositivo puente de AV Matrix existe
    double   avm_rate;               // frecuencia de AV Matrix
    bool     ext_present;            // el dispositivo externo está y responde
    bool     ext_is_avmatrix;        // es (o contiene) un dispositivo de AV Matrix: prohibido
    bool     ext_used_by_other;      // el otro puente usa el mismo dispositivo: prohibido
    uint32_t ext_channels;           // canales del lado usado: salidas (DVS) o entradas (EVO)
    double   ext_rate;
    char     ext_name[256];
    bool     suspended;
    uint32_t loopback_from;          // EVO: primer canal que es loopback (p. ej. 5 en la EVO8); 0 = no se sabe
} avm_bridge_env;

// Estado que corresponde a la configuración y al entorno, con un mensaje claro
// en `msg`. AVM_BRIDGE_RUN significa «debe funcionar». Para la EVO, un canal
// mayor que los que expone el dispositivo no impide funcionar: queda en
// silencio y el mensaje lo dice.
avm_bridge_state avm_bridge_evaluate(const avm_bridge_config* c, const avm_bridge_env* env, char* msg, size_t len);
const char* avm_bridge_state_label(avm_bridge_state s);

// --- Plan de copia (se prepara antes de arrancar; el hilo de IO solo lo lee,
// salvo el contador del fundido, que solo modifica él) -------------------------
typedef struct {
    int      in_nbufs, out_nbufs;
    uint32_t in_buf_ch[AVM_BRIDGE_MAX_BUFS];     // canales intercalados de cada buffer
    uint32_t out_buf_ch[AVM_BRIDGE_MAX_BUFS];
    int      ncopy;
    struct { int in_buf, in_ch, out_buf, out_ch; } copy[AVM_BRIDGE_MAX_CH];
    uint32_t fade_pos;                            // muestras de fundido ya hechas
    // Canales con los que se preparó el plan (para detectar cambios de configuración).
    uint32_t avm_in, avm_out, ext_in, ext_out;
    // El hilo de IO lo pone a 1 si recibe buffers con otra forma: el hilo
    // principal lo ve (avm_bridge_needs_rebuild) y reconstruye el puente.
    _Atomic uint32_t shape_mismatch;
} avm_bridge_plan;

// Lista de buffers con la MISMA disposición en memoria que AudioBufferList
// de CoreAudio (avm_bridge.c lo comprueba al compilar). Permite probar el
// callback real fuera de macOS.
typedef struct {
    uint32_t mNumberChannels;
    uint32_t mDataByteSize;
    void*    mData;
} avm_abl_buffer;

typedef struct {
    uint32_t       mNumberBuffers;
    avm_abl_buffer mBuffers[1];      // en realidad mNumberBuffers elementos
} avm_abl;

// Todo el trabajo del callback de IO del puente (hilo de audio, sin memoria
// nueva ni esperas):
//   * Si el número de buffers o sus canales no coinciden con el plan, o hay más
//     de AVM_BRIDGE_MAX_BUFS, entrega SILENCIO: cada buffer de salida se pone a
//     cero dentro de SU propio tamaño, y marca plan->shape_mismatch.
//   * Si coinciden, copia como mucho el menor número de fotogramas que caben en
//     TODOS los buffers con datos (entradas y salidas); lo que sobra de cada
//     salida se pone a cero. Un buffer con mData NULL o de tamaño 0 nunca se
//     escribe ni se lee (una entrada así cuenta como silencio).
// Devuelve los fotogramas copiados (0 si entregó silencio).
uint32_t avm_bridge_process(avm_bridge_plan* p, const avm_abl* in, avm_abl* out);

// ¿Hay que reconstruir el puente? true si los canales actuales del agregado
// (forma de los buffers) o del dispositivo externo no son los del plan, o si
// el hilo de IO recibió buffers con otra forma. Se llama en el hilo principal.
bool avm_bridge_needs_rebuild(const avm_bridge_plan* p,
                              const uint32_t* in_buf_ch, int in_nbufs, const uint32_t* out_buf_ch, int out_nbufs,
                              uint32_t ext_in, uint32_t ext_out);

// Canal global (desde 0) -> buffer y canal dentro del buffer.
bool avm_bridge_locate(const uint32_t* buf_ch, int nbufs, uint32_t global, int* buf, int* ch);

// Prepara el plan a partir de la forma de los buffers del agregado y de los
// canales del dispositivo de AV Matrix (avm_in/avm_out) y del externo
// (ext_in/ext_out). Comprueba que los totales cuadran con el orden esperado
// [AV Matrix, externo]. Devuelve false (y un motivo) si no se puede.
bool avm_bridge_make_plan(const avm_bridge_config* c,
                          const uint32_t* in_buf_ch, int in_nbufs, const uint32_t* out_buf_ch, int out_nbufs,
                          uint32_t avm_in, uint32_t avm_out, uint32_t ext_in, uint32_t ext_out,
                          avm_bridge_plan* p, char* why, size_t len);

// Copia un ciclo (hilo de IO): pone a cero todas las salidas y copia cada canal
// de su entrada a su salida, con un fundido de entrada las primeras
// AVM_BRIDGE_FADE_FRAMES muestras tras arrancar. `in[b]`/`out[b]` son buffers
// float intercalados con in_buf_ch[b]/out_buf_ch[b] canales; un puntero NULL
// se trata como silencio (entrada) o se ignora (salida).
void avm_bridge_render(avm_bridge_plan* p, uint32_t frames, const float* const* in, float* const* out);

#ifdef __cplusplus
}
#endif

#endif
