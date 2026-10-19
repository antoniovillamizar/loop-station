// Pruebas del mixer de AV Matrix (núcleo, sin CoreAudio): volumen y mute de
// fuentes (antes de la matriz) y destinos (después de la suma), rampas sin
// chasquidos, aislamiento, sumas, medidores, serialización y concurrencia.
// A 0 dB sin mute el audio debe ser idéntico bit a bit al de antes del mixer.

#include "../Driver/src/avm_core.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define MS    1000000ull
#define BLOCK 256
#define RAMP  ((int)AVM_GAIN_RAMP_FRAMES)

static avm_engine* new_engine(void)
{
    avm_engine* e = (avm_engine*)malloc(sizeof(avm_engine));
    avm_engine_init(e);
    return e;
}

// Matriz de prueba:
//   Smaart Salida N -> LISTENTO Envío N (N = 1..3)
//   Smaart Salida 4 + Salida 5 -> LISTENTO Envío 4 (suma)
//   LISTENTO Retorno 1 + Retorno 2 -> Smaart Entrada 1 (suma)
//   Música L -> Smaart Entrada 2
static void test_matrix(avm_matrix* m)
{
    avm_matrix_clear(m);
    for (int i = 0; i < 3; i++) m->cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;
    m->cell[AVM_DST_LINK_BASE + 3][AVM_SRC_SMAART_BASE + 3] = 1;
    m->cell[AVM_DST_LINK_BASE + 3][AVM_SRC_SMAART_BASE + 4] = 1;
    m->cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_LINK_BASE + 0] = 1;
    m->cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_LINK_BASE + 1] = 1;
    m->cell[AVM_DST_SMAART_BASE + 1][AVM_SRC_MUSIC_BASE + 0] = 1;
}

static float val_smaart(int c) { return (float)(c + 1) / 64.0f; }
static float val_link(int c)   { return -(float)(c + 1) / 32.0f; }

// Escribe valores constantes en los tres grupos de fuentes y lee los dos grupos
// de destinos en el mismo instante (sin retardo). Devuelve el nuevo instante.
static int64_t step_const(avm_engine* e, int64_t t, uint64_t* now, float smaart_out[BLOCK * 8], float link_out[BLOCK * 8])
{
    float sm[BLOCK * 8], li[BLOCK * 8], mu[BLOCK * 2];
    for (int f = 0; f < BLOCK; f++) {
        for (int c = 0; c < 8; c++) { sm[f * 8 + c] = val_smaart(c); li[f * 8 + c] = val_link(c); }
        mu[f * 2] = 0.25f; mu[f * 2 + 1] = -0.125f;
    }
    avm_engine_write_metered(e, AVM_SRC_GROUP_SMAART, t, BLOCK, sm, AVM_FMT_FLOAT32, 8, *now);
    avm_engine_write_metered(e, AVM_SRC_GROUP_LINK, t, BLOCK, li, AVM_FMT_FLOAT32, 8, *now);
    avm_engine_write_metered(e, AVM_SRC_GROUP_MUSIC, t, BLOCK, mu, AVM_FMT_FLOAT32, 2, *now);
    avm_engine_read_metered(e, AVM_DST_GROUP_SMAART, t, BLOCK, smaart_out, AVM_FMT_FLOAT32, 8, *now);
    avm_engine_read_metered(e, AVM_DST_GROUP_LINK, t, BLOCK, link_out, AVM_FMT_FLOAT32, 8, *now);
    *now += 5 * MS;
    return t + BLOCK;
}

// Avanza hasta que terminen todas las rampas (devuelve el último bloque leído).
static int64_t settle(avm_engine* e, int64_t t, uint64_t* now, float* so, float* lo)
{
    for (int i = 0; i < (RAMP / BLOCK) + 2; i++) t = step_const(e, t, now, so, lo);
    return t;
}

// Comparación campo a campo (memcmp incluiría el relleno de la estructura).
static int mixer_eq(const avm_mixer* a, const avm_mixer* b)
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++)
        if (a->src_gain[i] != b->src_gain[i] || a->src_mute[i] != b->src_mute[i]) return 0;
    for (int i = 0; i < AVM_NUM_DESTS; i++)
        if (a->dst_gain[i] != b->dst_gain[i] || a->dst_mute[i] != b->dst_mute[i]) return 0;
    return 1;
}

static uint32_t g_rng = 12345;
static uint32_t rnd(void) { g_rng = g_rng * 1664525u + 1013904223u; return g_rng; }
static float rndf(void) { return ((float)(rnd() >> 8) / 16777216.0f) * 2.0f - 1.0f; }

// --- Ajustes ------------------------------------------------------------------

static void test_defaults_and_validity(void)
{
    avm_engine* e = new_engine();
    avm_mixer mx;
    avm_engine_get_mixer(e, &mx);
    CHECK(avm_mixer_is_unity(&mx) && avm_mixer_is_valid(&mx), "instalación nueva: todos los canales a 0 dB y sin mute");
    int ok = 1;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (mx.src_gain[i] != 1.0f || mx.src_mute[i]) ok = 0;
    for (int i = 0; i < AVM_NUM_DESTS; i++) if (mx.dst_gain[i] != 1.0f || mx.dst_mute[i]) ok = 0;
    CHECK(ok, "34 canales: ganancia 1 exacta (0 dB) y mute 0");

    const float bad[] = { 1.0001f, 2.0f, -0.0001f, -1.0f, NAN, INFINITY, -INFINITY };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        avm_mixer b; avm_mixer_default(&b);
        b.src_gain[3] = bad[i];
        CHECK(!avm_mixer_is_valid(&b) && !avm_engine_set_mixer(e, &b), "ganancia de fuente %g rechazada (no se amplifica)", (double)bad[i]);
        avm_mixer_default(&b);
        b.dst_gain[9] = bad[i];
        CHECK(!avm_mixer_is_valid(&b) && !avm_engine_set_mixer(e, &b), "ganancia de destino %g rechazada", (double)bad[i]);
    }
    avm_mixer b; avm_mixer_default(&b);
    b.src_mute[0] = 2;
    CHECK(!avm_mixer_is_valid(&b), "mute distinto de 0/1 rechazado");
    avm_mixer_default(&b);
    b.src_gain[0] = 0.0f; b.dst_gain[0] = 1.0f; b.src_gain[1] = 0.5f; b.dst_mute[2] = 1;
    CHECK(avm_mixer_is_valid(&b) && !avm_mixer_is_unity(&b), "0, 0,5 y 1 son válidos");
    avm_engine_get_mixer(e, &mx);
    CHECK(avm_mixer_is_unity(&mx), "un ajuste rechazado no cambia nada");
    CHECK(avm_engine_set_mixer(e, &b), "ajuste válido aceptado");
    avm_engine_get_mixer(e, &mx);
    CHECK(mixer_eq(&mx, &b), "el motor devuelve los ajustes aplicados");
    CHECK(avm_mixer_src_effective(&b, 0) == 0.0f && avm_mixer_src_effective(&b, 1) == 0.5f && avm_mixer_dst_effective(&b, 2) == 0.0f
          && avm_mixer_dst_effective(&b, 3) == 1.0f, "ganancia efectiva: mute = 0, si no la ganancia");
    free(e);
}

static void test_db_conversion(void)
{
    CHECK(avm_db_to_gain(0.0f) == 1.0f, "0 dB = ganancia 1 exacta");
    CHECK(avm_db_to_gain(6.0f) == 1.0f && avm_db_to_gain(INFINITY) == 1.0f, "dB positivos limitados a 0 dB (sin amplificación)");
    CHECK(avm_db_to_gain(-INFINITY) == 0.0f && avm_db_to_gain(NAN) == 0.0f, "−∞ dB = silencio");
    CHECK(fabsf(avm_db_to_gain(-6.0206f) - 0.5f) < 1e-5f, "−6,02 dB ≈ 0,5");
    CHECK(fabsf(avm_db_to_gain(-20.0f) - 0.1f) < 1e-6f, "−20 dB = 0,1");
    CHECK(avm_gain_to_db(1.0f) == 0.0f && isinf(avm_gain_to_db(0.0f)) && avm_gain_to_db(0.0f) < 0, "ganancia 1 = 0 dB; 0 = −∞");
    CHECK(fabsf(avm_gain_to_db(avm_db_to_gain(-37.5f)) + 37.5f) < 1e-3f, "ida y vuelta dB -> ganancia -> dB");
}

static void test_pack_unpack(void)
{
    avm_mixer a; avm_mixer_default(&a);
    for (int i = 0; i < AVM_NUM_SOURCES; i++) { a.src_gain[i] = (float)i / 20.0f; a.src_mute[i] = (uint8_t)(i % 3 == 0); }
    for (int i = 0; i < AVM_NUM_DESTS; i++)   { a.dst_gain[i] = 1.0f - (float)i / 32.0f; a.dst_mute[i] = (uint8_t)(i % 2); }
    uint8_t blob[AVM_MIXER_BLOB_SIZE];
    avm_mixer_pack(&a, blob);
    avm_mixer b; avm_mixer_default(&b);
    CHECK(avm_mixer_unpack(blob, sizeof(blob), &b) && mixer_eq(&a, &b), "ida y vuelta de la serialización");
    CHECK(!memcmp(blob, "AVX1", 4), "cabecera AVX1");

    avm_mixer keep = b, out = b;
    CHECK(!avm_mixer_unpack(blob, sizeof(blob) - 1, &out) && mixer_eq(&out, &keep), "tamaño incorrecto rechazado sin modificar");
    uint8_t bad[AVM_MIXER_BLOB_SIZE];
    memcpy(bad, blob, sizeof(bad)); bad[0] = 'X';
    CHECK(!avm_mixer_unpack(bad, sizeof(bad), &out), "cabecera incorrecta rechazada");
    memcpy(bad, blob, sizeof(bad)); bad[16 + 5] = 1;
    CHECK(!avm_mixer_unpack(bad, sizeof(bad), &out), "relleno distinto de 0 rechazado");
    memcpy(bad, blob, sizeof(bad)); float big = 1.5f; memcpy(bad + 16, &big, 4);
    CHECK(!avm_mixer_unpack(bad, sizeof(bad), &out), "ganancia guardada > 1 rechazada");
    memcpy(bad, blob, sizeof(bad)); bad[16 + 4] = 7;
    CHECK(!avm_mixer_unpack(bad, sizeof(bad), &out) && mixer_eq(&out, &keep), "mute guardado inválido rechazado");
    CHECK(!avm_mixer_unpack(NULL, sizeof(blob), &out), "NULL rechazado");
}

// --- Audio ----------------------------------------------------------------------

// A 0 dB sin mute la salida es idéntica bit a bit a la de un motor que nunca
// tuvo mixer, en los cuatro formatos, con valores arbitrarios (incluidos −0,
// subnormales, ±1 y valores mayores que 1 en Float32).
static void test_unity_bit_identical(void)
{
    avm_matrix m; test_matrix(&m);
    for (int fmt = 0; fmt < AVM_FMT_COUNT; fmt++) {
        avm_engine* ref = new_engine();
        avm_engine* e = new_engine();
        avm_engine_set_matrix(ref, &m);
        avm_engine_set_matrix(e, &m);
        // El motor de prueba pasa antes por ajustes distintos y vuelve a 0 dB.
        avm_mixer mx; avm_mixer_default(&mx);
        for (int i = 0; i < AVM_NUM_SOURCES; i++) mx.src_gain[i] = 0.3f;
        for (int i = 0; i < AVM_NUM_DESTS; i++) mx.dst_mute[i] = 1;
        avm_engine_set_mixer(e, &mx);
        uint32_t bps = avm_format_bytes_per_sample((avm_format)fmt);
        uint8_t in_s[BLOCK * 8 * 4], in_l[BLOCK * 8 * 4], in_m[BLOCK * 2 * 4];
        uint8_t o1[BLOCK * 8 * 4], o2[BLOCK * 8 * 4];
        int64_t t = 0;
        int identical = 1, blocks = 0;
        for (int b = 0; b < 40; b++) {
            if (b == 8) { avm_mixer_default(&mx); avm_engine_set_mixer(e, &mx); }
            for (int i = 0; i < BLOCK * 8; i++) {
                float v = rndf();
                if (fmt == AVM_FMT_FLOAT32) {
                    switch (rnd() % 16) {
                        case 0: v = -0.0f; break;
                        case 1: v = 1e-40f; break;           // subnormal
                        case 2: v = 1.0f; break;
                        case 3: v = -1.0f; break;
                        case 4: v = 1.75f; break;            // > 0 dBFS en coma flotante
                        default: break;
                    }
                }
                avm_float_to_sample(v, in_s + (size_t)i * bps, (avm_format)fmt);
                avm_float_to_sample(rndf(), in_l + (size_t)i * bps, (avm_format)fmt);
            }
            for (int i = 0; i < BLOCK * 2; i++) avm_float_to_sample(rndf(), in_m + (size_t)i * bps, (avm_format)fmt);
            avm_engine* es[2] = { ref, e };
            for (int k = 0; k < 2; k++) {
                avm_engine_write(es[k], AVM_SRC_GROUP_SMAART, t, BLOCK, in_s, (avm_format)fmt, 8);
                avm_engine_write(es[k], AVM_SRC_GROUP_LINK, t, BLOCK, in_l, (avm_format)fmt, 8);
                avm_engine_write(es[k], AVM_SRC_GROUP_MUSIC, t, BLOCK, in_m, (avm_format)fmt, 2);
            }
            for (int g = 0; g < AVM_DST_GROUP_COUNT; g++) {
                avm_engine_read(ref, (avm_dst_group)g, t, BLOCK, o1, (avm_format)fmt, 8);
                avm_engine_read(e, (avm_dst_group)g, t, BLOCK, o2, (avm_format)fmt, 8);
                // Tras la rampa de vuelta (8 bloques + 512 muestras), idéntico.
                if (b >= 8 + RAMP / BLOCK + 1) { blocks++; if (memcmp(o1, o2, (size_t)BLOCK * 8 * bps)) identical = 0; }
            }
            t += BLOCK;
        }
        CHECK(identical && blocks > 0, "%s: a 0 dB sin mute, salida idéntica bit a bit a la de un motor sin mixer (%d bloques)",
              fmt == 0 ? "Float32" : fmt == 1 ? "SInt32" : fmt == 2 ? "SInt24" : "SInt16", blocks);
        free(ref); free(e);
    }
}

static void test_levels_isolation_and_sums(void)
{
    avm_matrix m; test_matrix(&m);
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_gain[AVM_SRC_SMAART_BASE + 1] = 0.5f;     // Salida 2 a −6 dB: Envío 2
    mx.dst_gain[AVM_DST_LINK_BASE + 3] = 0.25f;      // Envío 4 = (Salida 4 + Salida 5) · 0,25
    mx.src_gain[AVM_SRC_LINK_BASE + 0] = 0.5f;       // Entrada 1 = Retorno 1 · 0,5 + Retorno 2
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);   // sin IO todavía: los medidores no ven la rampa inicial
    float so[BLOCK * 8], lo[BLOCK * 8];
    uint64_t now = 1000 * MS;
    int64_t t = settle(e, 0, &now, so, lo);
    (void)t;
    int ok_unity = 1, ok_half = 1, ok_sum = 1, ok_presum = 1, ok_music = 1, ok_silent = 1;
    for (int f = 0; f < BLOCK; f++) {
        if (lo[f * 8 + 0] != val_smaart(0) || lo[f * 8 + 2] != val_smaart(2)) ok_unity = 0;
        if (lo[f * 8 + 1] != val_smaart(1) * 0.5f) ok_half = 0;
        if (lo[f * 8 + 3] != (val_smaart(3) + val_smaart(4)) * 0.25f) ok_sum = 0;
        if (so[f * 8 + 0] != val_link(0) * 0.5f + val_link(1)) ok_presum = 0;
        if (so[f * 8 + 1] != 0.25f) ok_music = 0;
        for (int c = 4; c < 8; c++) if (lo[f * 8 + c] != 0.0f) ok_silent = 0;
        for (int c = 2; c < 8; c++) if (so[f * 8 + c] != 0.0f) ok_silent = 0;
    }
    CHECK(ok_half, "volumen de entrada: Salida 2 a 0,5 -> Envío 2 = mitad exacta");
    CHECK(ok_unity, "aislamiento: Envío 1 y 3 siguen exactos a 0 dB");
    CHECK(ok_sum, "volumen de salida después de la suma: Envío 4 = (Salida 4 + Salida 5) · 0,25");
    CHECK(ok_presum, "volumen de entrada antes de la suma: Entrada 1 = Retorno 1 · 0,5 + Retorno 2");
    CHECK(ok_music, "Música L sin cambios a 0 dB");
    CHECK(ok_silent, "los destinos sin ruta siguen en silencio");

    // Medidores: muestran el nivel resultante de los controles.
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    avm_engine_meter_snapshot(e, now, s, d);
    CHECK(s[AVM_SRC_SMAART_BASE + 1].active && s[AVM_SRC_SMAART_BASE + 1].peak == val_smaart(1) * 0.5f, "medidor de la fuente: nivel tras su volumen");
    CHECK(s[AVM_SRC_SMAART_BASE + 0].peak == val_smaart(0), "medidor de otra fuente sin cambios");
    CHECK(d[AVM_DST_LINK_BASE + 3].peak == (val_smaart(3) + val_smaart(4)) * 0.25f, "medidor del destino: nivel tras la suma y su volumen");
    CHECK(d[AVM_DST_LINK_BASE + 1].peak == val_smaart(1) * 0.5f, "medidor del destino refleja el volumen de la fuente");
    free(e);
}

// Una fuente en mute que envía NaN o infinito (Float32) no contamina la suma:
// el mute entrega silencio exacto, no NaN · 0.
static void test_mute_blocks_non_finite(void)
{
    avm_matrix m; avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 1] = 1;
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_mute[1] = 1;
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);
    float in[BLOCK * 8], out[BLOCK * 8];
    for (int f = 0; f < BLOCK; f++) {
        for (int c = 0; c < 8; c++) in[f * 8 + c] = 0.0f;
        in[f * 8 + 0] = 0.25f;
        in[f * 8 + 1] = (f & 1) ? NAN : INFINITY;
    }
    avm_engine_write(e, AVM_SRC_GROUP_SMAART, 0, BLOCK, in, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, 0, BLOCK, out, AVM_FMT_FLOAT32, 8);
    int ok = 1;
    for (int f = 0; f < BLOCK; f++) if (out[f * 8] != 0.25f) ok = 0;
    CHECK(ok, "MUTE de una fuente con NaN/infinito: la suma conserva la otra fuente (0,25)");
    free(e);
}

static void test_mute_and_restore(void)
{
    avm_matrix m; test_matrix(&m);
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_gain[AVM_SRC_SMAART_BASE + 1] = 0.5f;
    mx.src_mute[AVM_SRC_SMAART_BASE + 1] = 1;        // fuente en mute (conserva 0,5)
    mx.dst_mute[AVM_DST_SMAART_BASE + 0] = 1;        // destino en mute (después de la suma)
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);
    float so[BLOCK * 8], lo[BLOCK * 8];
    uint64_t now = 1000 * MS;
    int64_t t = settle(e, 0, &now, so, lo);
    int ok = 1, other = 1;
    for (int f = 0; f < BLOCK; f++) {
        if (lo[f * 8 + 1] != 0.0f || so[f * 8 + 0] != 0.0f) ok = 0;
        if (lo[f * 8 + 0] != val_smaart(0) || so[f * 8 + 1] != 0.25f) other = 0;
    }
    CHECK(ok, "MUTE: silencio exacto en la fuente y en el destino");
    // Silencio con los mismos bits que un canal sin ruta (+0, no −0), aunque la
    // fuente sea negativa (LISTENTO envía valores negativos en esta prueba).
    int bits = 1;
    for (int f = 0; f < BLOCK; f++) if (signbit(so[f * 8 + 0]) || signbit(lo[f * 8 + 1])) bits = 0;
    CHECK(bits, "MUTE: silencio +0 exacto, idéntico al de un canal sin ruta");
    CHECK(other, "MUTE: los demás canales no cambian");
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    avm_engine_meter_snapshot(e, now, s, d);
    CHECK(s[AVM_SRC_SMAART_BASE + 1].active && s[AVM_SRC_SMAART_BASE + 1].peak == 0.0f, "medidor de una fuente en mute: activo y en silencio");
    CHECK(d[AVM_DST_SMAART_BASE + 0].active && d[AVM_DST_SMAART_BASE + 0].peak == 0.0f, "medidor de un destino en mute: activo y en silencio");

    mx.src_mute[AVM_SRC_SMAART_BASE + 1] = 0;
    mx.dst_mute[AVM_DST_SMAART_BASE + 0] = 0;
    avm_engine_set_mixer(e, &mx);
    settle(e, t, &now, so, lo);
    CHECK(lo[1] == val_smaart(1) * 0.5f, "quitar el MUTE vuelve al volumen anterior (0,5)");
    CHECK(so[0] == val_link(0) + val_link(1), "quitar el MUTE del destino vuelve a la suma completa");

    // «Silenciar todos»: los 34 canales en mute -> todo silencio.
    for (int i = 0; i < AVM_NUM_SOURCES; i++) mx.src_mute[i] = 1;
    for (int i = 0; i < AVM_NUM_DESTS; i++) mx.dst_mute[i] = 1;
    avm_engine_set_mixer(e, &mx);
    settle(e, t + 10 * BLOCK, &now, so, lo);
    int all = 1;
    for (int i = 0; i < BLOCK * 8; i++) if (so[i] != 0.0f || lo[i] != 0.0f) all = 0;
    CHECK(all, "silenciar todos: los 16 destinos en silencio exacto");
    // «Restaurar 0 dB y quitar mutes».
    avm_mixer_default(&mx);
    avm_engine_set_mixer(e, &mx);
    settle(e, t + 20 * BLOCK, &now, so, lo);
    CHECK(lo[1] == val_smaart(1) && so[0] == val_link(0) + val_link(1), "restaurar: todos a 0 dB y sin mute");
    free(e);
}

// Rampa: sin saltos al cambiar el volumen, de bloque a bloque y a mitad de rampa.
static void test_ramps_without_clicks(void)
{
    avm_matrix m; avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;   // por la fuente
    m.cell[AVM_DST_LINK_BASE + 1][AVM_SRC_SMAART_BASE + 1] = 1;   // por el destino
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    float in[BLOCK * 8], out[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) in[i] = 1.0f;
    float hist_s[12 * BLOCK], hist_d[12 * BLOCK];
    int64_t t = 0;
    avm_mixer mx; avm_mixer_default(&mx);
    for (int b = 0; b < 12; b++) {
        if (b == 2) { mx.src_mute[0] = 1; mx.dst_mute[AVM_DST_LINK_BASE + 1] = 1; avm_engine_set_mixer(e, &mx); }
        if (b == 6) { mx.src_mute[0] = 0; mx.dst_mute[AVM_DST_LINK_BASE + 1] = 0; avm_engine_set_mixer(e, &mx); }
        avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, in, AVM_FMT_FLOAT32, 8);
        avm_engine_read(e, AVM_DST_GROUP_LINK, t, BLOCK, out, AVM_FMT_FLOAT32, 8);
        for (int f = 0; f < BLOCK; f++) { hist_s[b * BLOCK + f] = out[f * 8]; hist_d[b * BLOCK + f] = out[f * 8 + 1]; }
        t += BLOCK;
    }
    const float* hs[2] = { hist_s, hist_d };
    const char* nm[2] = { "fuente", "destino" };
    for (int k = 0; k < 2; k++) {
        const float* h = hs[k];
        float maxjump = 0.0f;
        for (int i = 1; i < 12 * BLOCK; i++) { float j = fabsf(h[i] - h[i - 1]); if (j > maxjump) maxjump = j; }
        CHECK(maxjump <= 1.0f / (float)RAMP + 1e-6f, "%s: ningún salto mayor que 1/%d entre muestras (máximo %g)", nm[k], RAMP, (double)maxjump);
        int before = 1; for (int i = 0; i < 2 * BLOCK; i++) if (h[i] != 1.0f) before = 0;
        CHECK(before, "%s: antes del cambio, sin tocar", nm[k]);
        int down = 1; for (int i = 2 * BLOCK; i < 2 * BLOCK + RAMP; i++) if (!(h[i] < h[i - 1]) || h[i] < 0.0f) down = 0;
        CHECK(down, "%s: bajada progresiva durante %d muestras", nm[k], RAMP);
        CHECK(h[2 * BLOCK + RAMP - 1] == 0.0f && h[2 * BLOCK + RAMP - 2] > 0.0f, "%s: llega a silencio exacto justo al final de la rampa", nm[k]);
        int silent = 1; for (int i = 2 * BLOCK + RAMP; i < 6 * BLOCK; i++) if (h[i] != 0.0f) silent = 0;
        CHECK(silent, "%s: silencio exacto después de la rampa", nm[k]);
        int up = 1; for (int i = 6 * BLOCK; i < 6 * BLOCK + RAMP; i++) if (!(h[i] > h[i - 1]) || h[i] > 1.0f) up = 0;
        CHECK(up, "%s: subida progresiva al quitar el mute", nm[k]);
        int back = 1; for (int i = 6 * BLOCK + RAMP; i < 12 * BLOCK; i++) if (h[i] != 1.0f) back = 0;
        CHECK(back, "%s: vuelve exactamente a 0 dB", nm[k]);
    }
    CHECK((double)RAMP / 48000.0 > 0.005 && (double)RAMP / 44100.0 < 0.02, "rampa de %d muestras: entre 5 y 20 ms", RAMP);

    // Cambio a mitad de rampa: continúa desde el nivel actual, sin saltos.
    avm_engine_reset(e);
    avm_mixer_default(&mx); avm_engine_set_mixer(e, &mx);
    t = 0;
    float prev = 1.0f, maxjump = 0.0f;
    for (int b = 0; b < 10; b++) {
        if (b == 1) { mx.src_gain[0] = 0.0f; avm_engine_set_mixer(e, &mx); }
        if (b == 2) { mx.src_gain[0] = 0.8f; avm_engine_set_mixer(e, &mx); }   // a mitad de la bajada
        avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, in, AVM_FMT_FLOAT32, 8);
        avm_engine_read(e, AVM_DST_GROUP_LINK, t, BLOCK, out, AVM_FMT_FLOAT32, 8);
        for (int f = 0; f < BLOCK; f++) { float j = fabsf(out[f * 8] - prev); if (j > maxjump) maxjump = j; prev = out[f * 8]; }
        t += BLOCK;
    }
    CHECK(maxjump <= 1.0f / (float)RAMP + 1e-6f, "cambio a mitad de rampa sin saltos (máximo %g)", (double)maxjump);
    CHECK(prev == 0.8f, "termina en el nuevo nivel exacto");
    free(e);
}

// Nunca amplifica: con ganancias y señales aleatorias, |salida| <= |entrada|,
// también durante las rampas. Y no añade retardo: un impulso sale en el mismo instante.
static void test_no_gain_no_delay(void)
{
    avm_matrix m; avm_matrix_clear(&m);
    for (int i = 0; i < 8; i++) m.cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    float in[BLOCK * 8], out[BLOCK * 8];
    int64_t t = 0;
    int never_louder = 1;
    for (int b = 0; b < 60; b++) {
        if (b % 3 == 0) {
            avm_mixer mx; avm_mixer_default(&mx);
            for (int i = 0; i < AVM_NUM_SOURCES; i++) { mx.src_gain[i] = (float)(rnd() % 1001) / 1000.0f; mx.src_mute[i] = (uint8_t)(rnd() % 5 == 0); }
            for (int i = 0; i < AVM_NUM_DESTS; i++)   { mx.dst_gain[i] = (float)(rnd() % 1001) / 1000.0f; mx.dst_mute[i] = (uint8_t)(rnd() % 5 == 0); }
            avm_engine_set_mixer(e, &mx);
        }
        for (int i = 0; i < BLOCK * 8; i++) in[i] = rndf();
        avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, in, AVM_FMT_FLOAT32, 8);
        avm_engine_read(e, AVM_DST_GROUP_LINK, t, BLOCK, out, AVM_FMT_FLOAT32, 8);
        for (int i = 0; i < BLOCK * 8; i++) if (fabsf(out[i]) > fabsf(in[i])) never_louder = 0;
        t += BLOCK;
    }
    CHECK(never_louder, "con volúmenes aleatorios y rampas, ninguna muestra sale más fuerte que la de entrada");

    // Impulso en el instante 1000 con −6 dB en la fuente: sale en el mismo instante.
    avm_engine_reset(e);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_gain[0] = 0.5f;
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);   // sin IO: el volumen ya está aplicado
    float big_in[2048 * 8], big_out[2048 * 8];
    memset(big_in, 0, sizeof(big_in));
    big_in[1000 * 8] = 1.0f;
    avm_engine_write(e, AVM_SRC_GROUP_SMAART, 0, 2048, big_in, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, 0, 2048, big_out, AVM_FMT_FLOAT32, 8);
    int where = -1;
    for (int f = 0; f < 2048; f++) if (big_out[f * 8] != 0.0f) { where = f; break; }
    CHECK(where == 1000 && big_out[1000 * 8] == 0.5f, "sin retardo añadido: el impulso sale en el instante %d (esperado 1000) a 0,5", where);
    CHECK(e->src_gain[0].cur == 0.5f && e->src_gain[0].remaining == 0, "tras reiniciar sin IO, el volumen queda aplicado sin rampa");
    free(e);
}

// Saturación tras el volumen: una fuente a escala completa atenuada no satura.
static void test_clip_after_gain(void)
{
    avm_matrix m; avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    m.cell[AVM_DST_LINK_BASE + 1][AVM_SRC_SMAART_BASE + 1] = 1;
    avm_engine* e = new_engine();
    avm_engine_set_matrix(e, &m);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_gain[0] = 0.5f;
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);
    int16_t in[BLOCK * 8], out[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) in[i] = 32767;
    uint64_t now = 1000 * MS;
    avm_engine_write_metered(e, AVM_SRC_GROUP_SMAART, 0, BLOCK, in, AVM_FMT_SINT16, 8, now);
    avm_engine_read_metered(e, AVM_DST_GROUP_LINK, 0, BLOCK, out, AVM_FMT_SINT16, 8, now);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    avm_engine_meter_snapshot(e, now, s, d);
    CHECK(!s[0].clip && !d[AVM_DST_LINK_BASE].clip, "SInt16 a escala completa atenuado −6 dB: sin SAT en fuente ni destino");
    CHECK(s[1].clip && d[AVM_DST_LINK_BASE + 1].clip, "el mismo canal a 0 dB sí satura");
    CHECK(out[0] == 16384, "SInt16 32767 · 0,5 se entrega como 16384 (%d)", out[0]);
    free(e);
}

// Concurrencia: el hilo de control cambia el mixer sin parar mientras un
// escritor y un lector procesan audio (sin bloqueos; ThreadSanitizer sin avisos).
typedef struct { avm_engine* e; _Atomic int stop; _Atomic long bad; _Atomic long blocks; } conc;

static void* conc_io(void* p)
{
    conc* x = (conc*)p;
    float in[BLOCK * 8], out[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) in[i] = 0.5f;
    int64_t t = 0;
    while (!atomic_load(&x->stop)) {
        avm_engine_write(x->e, AVM_SRC_GROUP_SMAART, t, BLOCK, in, AVM_FMT_FLOAT32, 8);
        avm_engine_read(x->e, AVM_DST_GROUP_LINK, t, BLOCK, out, AVM_FMT_FLOAT32, 8);
        for (int i = 0; i < BLOCK * 8; i++) if (!(out[i] >= 0.0f && out[i] <= 0.5f)) atomic_fetch_add(&x->bad, 1);
        atomic_fetch_add(&x->blocks, 1);
        t += BLOCK;
    }
    return NULL;
}

static void test_concurrency(void)
{
    avm_matrix m; avm_matrix_clear(&m);
    for (int i = 0; i < 8; i++) m.cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;
    conc x;
    x.e = new_engine();
    avm_engine_set_matrix(x.e, &m);
    atomic_init(&x.stop, 0); atomic_init(&x.bad, 0); atomic_init(&x.blocks, 0);
    pthread_t io;
    pthread_create(&io, NULL, conc_io, &x);
    long changes = 0;
    while (atomic_load(&x.blocks) < 3000) {
        avm_mixer mx; avm_mixer_default(&mx);
        for (int i = 0; i < AVM_NUM_SOURCES; i++) { mx.src_gain[i] = (float)(rnd() % 101) / 100.0f; mx.src_mute[i] = (uint8_t)(rnd() & 1); }
        for (int i = 0; i < AVM_NUM_DESTS; i++)   { mx.dst_gain[i] = (float)(rnd() % 101) / 100.0f; mx.dst_mute[i] = (uint8_t)(rnd() & 1); }
        avm_engine_set_mixer(x.e, &mx);
        changes++;
    }
    atomic_store(&x.stop, 1);
    pthread_join(io, NULL);
    CHECK(atomic_load(&x.bad) == 0, "cambios continuos del mixer durante el audio: todas las muestras entre 0 y la entrada (%ld cambios)", changes);
    free(x.e);
}

int main(void)
{
    test_defaults_and_validity();
    test_db_conversion();
    test_pack_unpack();
    test_unity_bit_identical();
    test_levels_isolation_and_sums();
    test_mute_and_restore();
    test_mute_blocks_non_finite();
    test_ramps_without_clicks();
    test_no_gain_no_delay();
    test_clip_after_gain();
    test_concurrency();
    printf("Mixer: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
