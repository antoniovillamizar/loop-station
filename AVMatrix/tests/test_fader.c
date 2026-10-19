// Pruebas de la escala de los faders del mixer (App/avm_fader.c).

#include "../App/avm_fader.h"
#include "../Driver/src/avm_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_pass, g_fail;
#define CHECK(c, ...) do { if (c) g_pass++; else { g_fail++; printf("FALLO: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static int fmt_is(float g, const char* want)
{
    char b[32];
    avm_fader_format(g, b, sizeof(b));
    if (strcmp(b, want)) { printf("  formato de %g: «%s» (esperado «%s»)\n", (double)g, b, want); return 0; }
    return 1;
}

int main(void)
{
    CHECK(avm_fader_gain_from_pos(1.0) == 1.0f, "arriba = 0 dB exactos (ganancia 1)");
    CHECK(avm_fader_gain_from_pos(AVM_FADER_TOP_SNAP) == 1.0f && avm_fader_gain_from_pos(0.9995) == 1.0f, "cerca de arriba se ajusta a 0 dB exactos");
    CHECK(avm_fader_gain_from_pos(0.0) == 0.0f && avm_fader_gain_from_pos(-1.0) == 0.0f && avm_fader_gain_from_pos(NAN) == 0.0f, "abajo = silencio");
    CHECK(avm_fader_gain_from_pos(2.0) == 1.0f, "posición fuera de rango: sin amplificación");

    int mono = 1, never_loud = 1, valid = 1;
    float prev = -1.0f;
    for (int i = 0; i <= 10000; i++) {
        double p = i / 10000.0;
        float g = avm_fader_gain_from_pos(p);
        if (g < prev) mono = 0;
        if (g > 1.0f || g < 0.0f) never_loud = 0;
        avm_mixer mx; avm_mixer_default(&mx); mx.src_gain[0] = g;
        if (!avm_mixer_is_valid(&mx)) valid = 0;
        prev = g;
    }
    CHECK(mono, "la ganancia nunca baja al subir el fader");
    CHECK(never_loud, "ganancia siempre entre 0 y 1 (solo atenuación)");
    CHECK(valid, "toda posición da un ajuste que el driver acepta");

    double p6 = avm_fader_pos_from_gain(0.5f), p20 = avm_fader_pos_from_gain(0.1f), p60 = avm_fader_pos_from_gain(0.001f);
    CHECK(p6 > 0.75 && p6 < 0.82 && p20 > 0.4 && p20 < 0.5 && p60 > 0.09 && p60 < 0.11,
          "escala útil: −6 dB al %.0f %%, −20 dB al %.0f %%, −60 dB al %.0f %%", p6 * 100, p20 * 100, p60 * 100);
    int round = 1;
    for (int i = 1; i < 990; i++) {
        double p = i / 1000.0;
        if (fabs(avm_fader_pos_from_gain(avm_fader_gain_from_pos(p)) - p) > 1e-6) round = 0;
    }
    CHECK(round, "ida y vuelta posición -> ganancia -> posición");
    CHECK(avm_fader_pos_from_gain(1.0f) == 1.0 && avm_fader_pos_from_gain(0.0f) == 0.0, "0 dB arriba del todo, silencio abajo del todo");

    CHECK(fmt_is(1.0f, "0.0 dB"), "lectura de 0 dB");
    CHECK(fmt_is(0.0f, "−∞"), "lectura de silencio");
    CHECK(fmt_is(0.5f, "-6.0 dB"), "lectura de −6 dB");
    CHECK(fmt_is(0.1f, "-20.0 dB"), "lectura de −20 dB");
    CHECK(fmt_is(0.9995f, "0.0 dB"), "sin «-0.0 dB»");
    CHECK(fmt_is(avm_db_to_gain(-12.5f), "-12.5 dB"), "lectura de −12,5 dB");
    char tiny[4];
    avm_fader_format(0.5f, tiny, sizeof(tiny));
    CHECK(strlen(tiny) < sizeof(tiny), "búfer corto: sin desbordamiento");

    printf("Faders: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
