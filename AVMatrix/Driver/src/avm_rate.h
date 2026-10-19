// AV Matrix — transacción de cambio de frecuencia de muestreo (C puro).
//
// Los tres dispositivos comparten reloj y frecuencia. Un cambio de frecuencia
// afecta a los tres, y CoreAudio lo aplica dispositivo por dispositivo
// (RequestDeviceConfigurationChange → PerformDeviceConfigurationChange o
// AbortDeviceConfigurationChange). Para que ningún dispositivo funcione nunca
// a una frecuencia distinta de la que el host cree:
//
//   * El cambio solo se admite si NO hay IO activa en ninguno de los tres.
//   * Mientras la transacción está abierta, StartIO se rechaza en los tres.
//   * Si el host cancela (Abort) o falla la petición después de que algún
//     dispositivo ya aplicó la nueva frecuencia, se abre una transacción de
//     reversión a la frecuencia anterior.
//   * Una transacción abandonada (sin Perform ni Abort) caduca tras un tiempo.
//   * Si los dispositivos pueden haber quedado a frecuencias distintas (reversión
//     cancelada o fallida, cambio o reversión caducados a medias), el estado
//     queda INCONSISTENTE: StartIO se rechaza en los tres hasta una recuperación.
//   * Recuperación: pedir cualquier frecuencia admitida, incluida la actual
//     (en estado inconsistente no se responde SAME). Solo cuando los tres
//     dispositivos la aplican se vuelve a permitir el arranque de audio.
//
// Este módulo no tiene bloqueos: el llamador lo protege con un mutex.

#ifndef AVM_RATE_H
#define AVM_RATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AVM_RATE_MAX_DEVICES 8

typedef enum {
    AVM_RATE_OK = 0,
    AVM_RATE_SAME,          // ya está a esa frecuencia: nada que hacer
    AVM_RATE_UNSUPPORTED,   // frecuencia no admitida
    AVM_RATE_IO_ACTIVE,     // hay IO en algún dispositivo: rechazado
    AVM_RATE_BUSY,          // ya hay una transacción en curso
    AVM_RATE_STALE,         // petición/aplicación de una transacción que ya no es la vigente
    AVM_RATE_INCONSISTENT   // los dispositivos pueden estar a frecuencias distintas: hace falta recuperar
} avm_rate_status;

typedef struct {
    double   current_rate;      // frecuencia vigente del driver
    double   previous_rate;     // frecuencia antes de la transacción abierta
    double   pending_rate;      // 0 = sin transacción
    uint32_t generation;        // identifica la transacción vigente (24 bits útiles)
    uint32_t performed_mask;    // dispositivos que ya aplicaron la transacción vigente
    uint32_t device_count;
    bool     is_revert;         // la transacción vigente es una reversión
    bool     inconsistent;      // posible discrepancia de frecuencia: bloquea StartIO hasta recuperar
    uint64_t started_at;        // instante de apertura (unidades del llamador)
    uint64_t timeout;           // caducidad de una transacción abandonada
    uint32_t running[AVM_RATE_MAX_DEVICES];
    uint32_t running_total;
} avm_rate_state;

typedef struct {
    bool     revert_needed;     // el llamador debe pedir la reversión a todos los dispositivos
    double   revert_rate;
    uint32_t revert_generation;
} avm_rate_abort_result;

bool avm_rate_is_supported(double rate);

void avm_rate_init(avm_rate_state* st, uint32_t device_count, double rate, uint64_t timeout);

// Abre una transacción. Si devuelve AVM_RATE_OK, *out_generation identifica la transacción.
avm_rate_status avm_rate_begin(avm_rate_state* st, double rate, uint64_t now, uint32_t* out_generation);

// El host autoriza a `device` a aplicar la transacción `generation`.
avm_rate_status avm_rate_perform(avm_rate_state* st, uint32_t device, uint32_t generation, double rate);

// El host cancela, o la petición falló, para `device` en la transacción `generation`.
avm_rate_abort_result avm_rate_abort(avm_rate_state* st, uint32_t device, uint32_t generation, uint64_t now);

// Arranque/parada de IO de un dispositivo. avm_rate_io_start no cuenta el arranque y
// devuelve AVM_RATE_BUSY mientras haya una transacción abierta y no caducada, o
// AVM_RATE_INCONSISTENT mientras los dispositivos puedan estar a frecuencias distintas.
avm_rate_status avm_rate_io_start(avm_rate_state* st, uint32_t device, uint64_t now);
void avm_rate_io_stop(avm_rate_state* st, uint32_t device);

bool avm_rate_pending(const avm_rate_state* st);

// Estado para diagnóstico y para la interfaz: 0 = coherente, 1 = cambio en curso,
// 2 = inconsistente (recuperar pidiendo una frecuencia). Aplica antes la caducidad.
int avm_rate_health(avm_rate_state* st, uint64_t now);

#ifdef __cplusplus
}
#endif

#endif
