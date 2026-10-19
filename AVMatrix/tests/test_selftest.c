// Pruebas del veredicto de la autoprueba de avmctl (Client/avm_selftest.h):
// un fallo al restaurar el mixer (o la matriz, la frecuencia o los formatos)
// debe terminar con FALLO y código 1, aunque las comprobaciones de audio pasen.

#include "../Client/avm_selftest.h"

#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    // Las 32 combinaciones: código 0 y «CORRECTO» solo con todo verdadero.
    int ok = 1;
    for (int m = 0; m < 32; m++) {
        avm_selftest_outcome o = { m & 1, (m >> 1) & 1, (m >> 2) & 1, (m >> 3) & 1, (m >> 4) & 1 };
        int code = avm_selftest_exit_code(&o);
        const char* v = avm_selftest_verdict(&o);
        int want0 = m == 31;
        if ((code == 0) != want0 || (strcmp(v, "CORRECTO") == 0) != want0) ok = 0;
    }
    CHECK(ok, "código 0 y CORRECTO solo si todo pasó y todo se restauró (32 combinaciones)");

    avm_selftest_outcome mixer_fail = { true, true, true, false, true };
    CHECK(avm_selftest_exit_code(&mixer_fail) == 1, "audio correcto pero falla restaurar el mixer: código 1");
    CHECK(strstr(avm_selftest_verdict(&mixer_fail), "no se pudo restaurar") != NULL, "el veredicto explica que no se pudo restaurar");
    avm_selftest_outcome matrix_fail = { true, true, true, true, false };
    CHECK(avm_selftest_exit_code(&matrix_fail) == 1, "falla restaurar la matriz: código 1");
    avm_selftest_outcome rate_fail = { true, false, true, true, true };
    CHECK(avm_selftest_exit_code(&rate_fail) == 1, "falla restaurar la frecuencia: código 1");
    avm_selftest_outcome fmt_fail = { true, true, false, true, true };
    CHECK(avm_selftest_exit_code(&fmt_fail) == 1, "falla restaurar un formato: código 1");
    avm_selftest_outcome audio_fail = { false, true, true, true, true };
    CHECK(avm_selftest_exit_code(&audio_fail) == 1 && !strcmp(avm_selftest_verdict(&audio_fail), "HAY FALLOS"), "fallo de audio: HAY FALLOS");
    avm_selftest_outcome all = { true, true, true, true, true };
    CHECK(avm_selftest_exit_code(&all) == 0 && !strcmp(avm_selftest_verdict(&all), "CORRECTO"), "todo bien: CORRECTO y código 0");

    printf("Autoprueba (veredicto): %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
