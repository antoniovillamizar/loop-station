// AV Matrix — puentes Dante y EVO con CoreAudio (dentro de la app).
//
// Cada puente crea un dispositivo agregado PRIVADO (solo visible para la app,
// desaparece con ella) con [dispositivo puente de AV Matrix, dispositivo
// externo]. Reloj: el del externo (DVS o la EVO). El lado de AV Matrix lleva
// compensación de deriva, así CoreAudio adapta los dos relojes sin que
// ninguno de los dos se adelante ni se quede atrás. Un IOProc copia los
// canales (avm_bridge_core.c) sin esperas ni memoria nueva.
//
// Todo se llama desde el hilo principal. Los cambios del sistema (dispositivo
// que aparece o desaparece, frecuencias) llegan por avisos de CoreAudio en la
// cola principal: el puente se detiene o se reconecta solo y llama a `notify`.
// Cada arranque crea un agregado nuevo: nunca se reproduce audio viejo.

#ifndef AVM_BRIDGE_H
#define AVM_BRIDGE_H

#include <CoreAudio/CoreAudio.h>

#include "avm_bridge_core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avm_bridge avm_bridge;
typedef void (*avm_bridge_notify)(void* ctx);

avm_bridge* avm_bridge_create(avm_bridge_kind kind, avm_bridge_notify notify, void* ctx);
void        avm_bridge_destroy(avm_bridge* b);                 // detiene y libera

// Aplica una configuración (detiene si cambia el dispositivo o los canales) y re-evalúa.
void        avm_bridge_configure(avm_bridge* b, const avm_bridge_config* c);
// UID del dispositivo que usa el otro puente (para impedir realimentación).
void        avm_bridge_set_other_uid(avm_bridge* b, const char* uid);
// Pausa (p. ej. mientras AV Matrix cambia de frecuencia) y reanudación.
void        avm_bridge_set_suspended(avm_bridge* b, bool suspended);
// Vuelve a mirar el sistema y arranca o detiene según corresponda.
void        avm_bridge_refresh(avm_bridge* b);
// Comprobación periódica (hilo principal, p. ej. con el temporizador de los
// medidores): si el hilo de IO recibió buffers con otra forma, reconstruye.
void        avm_bridge_poll(avm_bridge* b);

avm_bridge_state avm_bridge_get_state(const avm_bridge* b, char* msg, size_t len);
bool        avm_bridge_is_running(const avm_bridge* b);
const avm_bridge_config* avm_bridge_get_config(const avm_bridge* b);
// Pone el dispositivo externo a la frecuencia de AV Matrix (si el dispositivo lo permite).
OSStatus    avm_bridge_match_rate(avm_bridge* b);

#ifdef __cplusplus
}
#endif

#endif
