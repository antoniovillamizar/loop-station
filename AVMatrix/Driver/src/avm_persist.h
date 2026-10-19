// AV Matrix — guardado ordenado de ajustes (C puro con pthreads, sin CoreAudio).
//
// Problema que evita: dos cambios simultáneos A y B pueden APLICARSE en orden A,
// B (bajo el mutex de estado) pero GUARDARSE en orden B, A si el guardado se
// hace fuera de ese mutex. El estado guardado quedaría con A aunque el vigente
// sea B (p. ej. MUTE vigente = 1, MUTE guardado = 0).
//
// Solución: cada cambio se aplica bajo `state` y recibe una generación. Después,
// bajo `store` (que serializa los guardados), se vuelve a leer el estado VIGENTE
// bajo `state` y se guarda ese, solo si es más nuevo que el último guardado. El
// último guardado siempre contiene el último cambio aplicado. El hilo de audio
// no toma ninguno de los dos mutex, y el almacenamiento (lento) no se escribe
// con `state` tomado.

#ifndef AVM_PERSIST_H
#define AVM_PERSIST_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    pthread_mutex_t* state;   // protege el estado (compartido, p. ej. gStateMutex)
    pthread_mutex_t  store;   // serializa las escrituras al almacenamiento
    uint64_t         applied; // generación aplicada (bajo state)
    uint64_t         stored;  // generación guardada (bajo store)
    // Solo para pruebas: se llama tras aplicar y antes de guardar (NULL = nada).
    void           (*after_apply)(void* ctx);
} avm_persist;

void avm_persist_init(avm_persist* p, pthread_mutex_t* state_mutex);

// apply(obj, value): aplica el valor; false = rechazado (no se guarda nada).
// snapshot(obj, out): copia el estado vigente (se llama con `state` tomado).
// write(snap, ctx): guarda una copia (se llama con `store` tomado, sin `state`).
// `scratch` es un búfer del tamaño del estado para la copia.
bool avm_persist_commit(avm_persist* p,
                        bool (*apply)(void* obj, const void* value),
                        void (*snapshot)(void* obj, void* out),
                        void (*write)(const void* snap, void* ctx),
                        void* obj, const void* value, void* scratch, void* ctx);

#ifdef __cplusplus
}
#endif

#endif
