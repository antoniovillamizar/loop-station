// AV Matrix — veredicto de la autoprueba de avmctl (C puro, probado en Linux).
//
// La autoprueba cambia la matriz, el mixer, la frecuencia y los formatos, y al
// terminar los restaura. Si cualquier restauración falla, el Mac queda con un
// estado distinto del que tenía el usuario: la autoprueba debe terminar con
// FALLO (código 1) aunque todas las comprobaciones de audio hayan pasado.

#ifndef AVM_SELFTEST_H
#define AVM_SELFTEST_H

#include <stdbool.h>

typedef struct {
    bool tests_passed;   // todas las comprobaciones de audio y medidores
    bool rate_restored;
    bool formats_restored;
    bool mixer_restored;
    bool matrix_restored;
} avm_selftest_outcome;

static inline bool avm_selftest_restored(const avm_selftest_outcome* o)
{
    return o->rate_restored && o->formats_restored && o->mixer_restored && o->matrix_restored;
}

static inline bool avm_selftest_ok(const avm_selftest_outcome* o)
{
    return o->tests_passed && avm_selftest_restored(o);
}

// Código de salida del proceso: 0 solo si todo pasó y todo se restauró.
static inline int avm_selftest_exit_code(const avm_selftest_outcome* o)
{
    return avm_selftest_ok(o) ? 0 : 1;
}

static inline const char* avm_selftest_verdict(const avm_selftest_outcome* o)
{
    if (!o->tests_passed) return "HAY FALLOS";
    if (!avm_selftest_restored(o)) return "HAY FALLOS (no se pudo restaurar el estado anterior)";
    return "CORRECTO";
}

#endif
