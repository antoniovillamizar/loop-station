// Pruebas de los medidores de nivel de AV Matrix (núcleo, sin CoreAudio).

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

#define SEC 1000000000ull
#define MS  1000000ull
#define BLOCK 256

static avm_engine* new_engine(void)
{
    avm_engine* e = (avm_engine*)malloc(sizeof(avm_engine));
    avm_engine_init(e);
    return e;
}

// Escribe en un grupo de fuentes un valor constante por canal (val[c]) durante n bloques.
static int64_t write_const(avm_engine* e, avm_src_group g, int64_t t, int blocks, const float* val, uint64_t* now)
{
    int nch = avm_src_group_channels(g);
    float buf[BLOCK * 8];
    for (int b = 0; b < blocks; b++) {
        for (int f = 0; f < BLOCK; f++)
            for (int c = 0; c < nch; c++) buf[f * nch + c] = val[c];
        avm_engine_write_metered(e, g, t, BLOCK, buf, AVM_FMT_FLOAT32, (uint32_t)nch, *now);
        t += BLOCK;
        *now += 5 * MS;
    }
    return t;
}

static void read_group(avm_engine* e, avm_dst_group g, int64_t t, uint64_t now, float* out)
{
    avm_engine_read_metered(e, g, t, BLOCK, out, AVM_FMT_FLOAT32, 8, now);
}

static void snap(avm_engine* e, uint64_t now, avm_meter_reading* s, avm_meter_reading* d)
{
    avm_engine_meter_snapshot(e, now, s, d);
}

static void test_never_written_and_silence(void)
{
    avm_engine* e = new_engine();
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, 10 * SEC, s, d);
    int ok = 1;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (s[i].active || s[i].peak != 0 || s[i].clip) ok = 0;
    for (int i = 0; i < AVM_NUM_DESTS; i++) if (d[i].active || d[i].peak != 0 || d[i].clip) ok = 0;
    CHECK(ok, "sin audio nunca: todos inactivos, pico 0, sin saturación");

    uint64_t now = 10 * SEC;
    float zero[8] = { 0 };
    int64_t t = write_const(e, AVM_SRC_GROUP_SMAART, 0, 4, zero, &now);
    float out[BLOCK * 8];
    read_group(e, AVM_DST_GROUP_LINK, t - 2 * BLOCK, now, out);
    snap(e, now, s, d);
    ok = 1;
    for (int c = 0; c < 8; c++) if (!s[AVM_SRC_SMAART_BASE + c].active || s[AVM_SRC_SMAART_BASE + c].peak != 0) ok = 0;
    CHECK(ok, "silencio digital: fuentes Smaart activas con pico 0");
    ok = 1;
    for (int c = 0; c < 8; c++) if (!d[AVM_DST_LINK_BASE + c].active || d[AVM_DST_LINK_BASE + c].peak != 0) ok = 0;
    CHECK(ok, "silencio digital: destinos LISTENTO activos con pico 0");
    CHECK(isinf(avm_peak_to_dbfs(0.0f)) && avm_peak_to_dbfs(0.0f) < 0, "pico 0 = -inf dBFS");
    CHECK(!s[AVM_SRC_LINK_BASE].active && !s[AVM_SRC_MUSIC_BASE].active, "las fuentes que no envían siguen inactivas");
    free(e);
}

static void test_known_signal(void)
{
    avm_engine* e = new_engine();
    uint64_t now = 20 * SEC;
    // Seno de 1 kHz a 48 kHz con pico 0,5 (-6,02 dBFS) en Smaart Salida 1.
    float buf[BLOCK * 8];
    float max_written = 0;
    int64_t t = 0;
    for (int b = 0; b < 8; b++) {
        for (int f = 0; f < BLOCK; f++) {
            float v = 0.5f * sinf(2.0f * 3.14159265358979f * 1000.0f * (float)(t + f) / 48000.0f);
            for (int c = 0; c < 8; c++) buf[f * 8 + c] = c == 0 ? v : 0.0f;
            if (fabsf(v) > max_written) max_written = fabsf(v);
        }
        avm_engine_write_metered(e, AVM_SRC_GROUP_SMAART, t, BLOCK, buf, AVM_FMT_FLOAT32, 8, now);
        t += BLOCK;
        now += 5 * MS;
    }
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    CHECK(s[0].active && s[0].peak == max_written, "seno 0,5: el pico medido es el pico enviado (%.7f vs %.7f)", s[0].peak, max_written);
    CHECK(fabsf(avm_peak_to_dbfs(0.5f) - (-6.0206f)) < 0.001f, "0,5 = -6,02 dBFS (%.4f)", avm_peak_to_dbfs(0.5f));
    CHECK(fabsf(avm_peak_to_dbfs(s[0].peak) + 6.02f) < 0.05f, "lectura del seno ≈ -6,02 dBFS (%.3f)", avm_peak_to_dbfs(s[0].peak));
    CHECK(!s[0].clip, "un seno a -6 dBFS no satura");
    // Formato entero: 16384 en SInt16 = 0,5.
    int16_t i16[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) i16[i] = (int16_t)((i % 8) == 1 ? 16384 : 0);
    avm_engine_write_metered(e, AVM_SRC_GROUP_SMAART, t, BLOCK, i16, AVM_FMT_SINT16, 8, now);
    snap(e, now, s, d);
    CHECK(s[1].peak == 0.5f, "SInt16 16384 se mide como 0,5 (%.7f)", s[1].peak);
    free(e);
}

static void test_channel_separation(void)
{
    avm_engine* e = new_engine();
    uint64_t now = 30 * SEC;
    float sm[8], li[8], mu[2] = { 0.125f, 0.0625f };
    for (int c = 0; c < 8; c++) { sm[c] = (float)(c + 1) / 16.0f; li[c] = -(float)(c + 1) / 64.0f; }
    write_const(e, AVM_SRC_GROUP_SMAART, 0, 3, sm, &now);
    write_const(e, AVM_SRC_GROUP_LINK, 0, 3, li, &now);
    write_const(e, AVM_SRC_GROUP_MUSIC, 0, 3, mu, &now);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    int ok = 1;
    for (int c = 0; c < 8; c++) {
        if (s[AVM_SRC_SMAART_BASE + c].peak != sm[c]) ok = 0;
        if (s[AVM_SRC_LINK_BASE + c].peak != fabsf(li[c])) ok = 0;
    }
    if (s[AVM_SRC_MUSIC_BASE].peak != 0.125f || s[AVM_SRC_MUSIC_BASE + 1].peak != 0.0625f) ok = 0;
    CHECK(ok, "cada medidor de fuente muestra solo su canal (18 valores distintos)");
    free(e);
}

static void test_destinations_after_routing(void)
{
    avm_engine* e = new_engine();   // preset de medición
    uint64_t now = 40 * SEC;
    float sm[8], li[8];
    for (int c = 0; c < 8; c++) { sm[c] = (float)(c + 1) / 16.0f; li[c] = (float)(c + 1) / 64.0f; }
    int64_t t = 0;
    for (int b = 0; b < 4; b++) {
        write_const(e, AVM_SRC_GROUP_SMAART, t, 1, sm, &now);
        write_const(e, AVM_SRC_GROUP_LINK, t, 1, li, &now);
        t += BLOCK;
    }
    float out[BLOCK * 8];
    read_group(e, AVM_DST_GROUP_SMAART, BLOCK, now, out);
    read_group(e, AVM_DST_GROUP_LINK, BLOCK, now, out);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    int ok = 1;
    for (int c = 0; c < 7; c++) if (d[AVM_DST_SMAART_BASE + c].peak != li[c]) ok = 0;
    CHECK(ok, "Smaart Entradas 1-7 miden el retorno de LISTENTO enrutado");
    CHECK(d[AVM_DST_LOCAL_REFERENCE].peak == sm[0], "Entrada 8 (referencia local) mide Smaart Salida 1 (%.4f), no el retorno 8 (%.4f)",
          d[AVM_DST_LOCAL_REFERENCE].peak, li[7]);
    ok = 1;
    for (int c = 0; c < 8; c++) if (d[AVM_DST_LINK_BASE + c].peak != sm[c]) ok = 0;
    CHECK(ok, "LISTENTO Envíos 1-8 miden Smaart Salidas 1-8");

    // Sin conexiones: el destino entrega silencio aunque la fuente tenga señal.
    avm_matrix m;
    avm_matrix_clear(&m);
    avm_engine_set_matrix(e, &m);
    // Se sigue enviando y leyendo durante 150 ms (más de las 2 ventanas de 50 ms que se retienen).
    for (int i = 0; i < 30; i++) {
        write_const(e, AVM_SRC_GROUP_SMAART, t, 1, sm, &now);
        read_group(e, AVM_DST_GROUP_LINK, t - BLOCK, now, out);
        t += BLOCK;
    }
    snap(e, now, s, d);
    CHECK(d[AVM_DST_LINK_BASE].active && d[AVM_DST_LINK_BASE].peak < 1e-9f && s[AVM_SRC_SMAART_BASE].peak > 0,
          "matriz vacía: el destino mide silencio mientras la fuente tiene señal (no repite la fuente)");
    free(e);
}

static void test_destination_sum(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    m.cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_LINK_BASE + 0] = 1;  // 0,25
    m.cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_LINK_BASE + 1] = 1;  // + 0,5
    m.cell[AVM_DST_SMAART_BASE + 1][AVM_SRC_LINK_BASE + 0] = 1;  // 0,25
    m.cell[AVM_DST_SMAART_BASE + 1][AVM_SRC_LINK_BASE + 2] = 1;  // + (-0,125)
    avm_engine_set_matrix(e, &m);
    uint64_t now = 50 * SEC;
    float li[8] = { 0.25f, 0.5f, -0.125f, 0, 0, 0, 0, 0 };
    write_const(e, AVM_SRC_GROUP_LINK, 0, 4, li, &now);
    float out[BLOCK * 8];
    read_group(e, AVM_DST_GROUP_SMAART, BLOCK, now, out);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    CHECK(d[0].peak == 0.75f, "suma en fase: 0,25 + 0,5 = 0,75 en el destino (%.4f)", d[0].peak);
    CHECK(d[1].peak == 0.125f, "suma en contrafase: 0,25 - 0,125 = 0,125 (%.4f)", d[1].peak);
    CHECK(d[0].peak != s[AVM_SRC_LINK_BASE].peak && d[0].peak != s[AVM_SRC_LINK_BASE + 1].peak,
          "el destino no repite ninguna fuente: mide la suma real");
    CHECK(out[0] == 0.75f && out[1] == 0.125f, "las muestras entregadas coinciden con lo medido");
    free(e);
}

static void test_clip(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_SMAART_BASE + 1] = 1;   // 0,6 + 0,6 = 1,2
    m.cell[AVM_DST_LINK_BASE + 1][AVM_SRC_SMAART_BASE + 2] = 1;   // 1,0
    m.cell[AVM_DST_LINK_BASE + 2][AVM_SRC_SMAART_BASE + 3] = 1;   // 0,99
    avm_engine_set_matrix(e, &m);
    uint64_t now = 60 * SEC;
    float sm[8] = { 0.6f, 0.6f, 1.0f, 0.99f, 0, 0, 0, 0 };
    write_const(e, AVM_SRC_GROUP_SMAART, 0, 4, sm, &now);
    float out[BLOCK * 8];
    read_group(e, AVM_DST_GROUP_LINK, BLOCK, now, out);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    CHECK(!s[0].clip && !s[1].clip, "fuentes a 0,6: sin saturación");
    CHECK(s[2].clip, "fuente a 1,0 (0 dBFS): saturación");
    CHECK(!s[3].clip, "fuente a 0,99: sin saturación");
    CHECK(d[AVM_DST_LINK_BASE].clip && fabsf(d[AVM_DST_LINK_BASE].peak - 1.2f) < 1e-6f,
          "destino que suma 0,6 + 0,6: pico 1,2 (+1,6 dBFS) y saturación, aunque ninguna fuente satura");
    CHECK(d[AVM_DST_LINK_BASE + 1].clip && !d[AVM_DST_LINK_BASE + 2].clip, "destinos a 1,0 y 0,99");

    // SInt16 a escala completa satura; un código menos no.
    int16_t i16[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) i16[i] = (int16_t)((i % 8) == 4 ? 32767 : ((i % 8) == 5 ? 32766 : 0));
    avm_engine_write_metered(e, AVM_SRC_GROUP_SMAART, 4 * BLOCK, BLOCK, i16, AVM_FMT_SINT16, 8, now);
    snap(e, now, s, d);
    CHECK(s[4].clip && !s[5].clip, "SInt16: 32767 satura, 32766 no");

    // El indicador se mantiene 2 s y luego se apaga solo.
    float quiet[8] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0, 0 };
    uint64_t clip_time = now;
    now = clip_time + AVM_METER_CLIP_HOLD_NS - 100 * MS;
    write_const(e, AVM_SRC_GROUP_SMAART, 5 * BLOCK, 1, quiet, &now);
    snap(e, now, s, d);
    CHECK(s[2].clip, "la saturación se mantiene visible durante 2 s");
    now = clip_time + AVM_METER_CLIP_HOLD_NS + 100 * MS;
    write_const(e, AVM_SRC_GROUP_SMAART, 6 * BLOCK, 1, quiet, &now);
    snap(e, now, s, d);
    CHECK(!s[2].clip, "pasados 2 s sin saturar, el indicador se apaga");
    free(e);
}

static void test_no_stale_values(void)
{
    avm_engine* e = new_engine();
    uint64_t now = 70 * SEC;
    float loud[8] = { 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f, 0.9f };
    int64_t t = write_const(e, AVM_SRC_GROUP_SMAART, 0, 4, loud, &now);
    float out[BLOCK * 8];
    read_group(e, AVM_DST_GROUP_LINK, t - 2 * BLOCK, now, out);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    snap(e, now, s, d);
    CHECK(s[0].peak == 0.9f && d[AVM_DST_LINK_BASE].peak == 0.9f, "con audio: 0,9 en fuente y destino");

    // Se detiene el audio: sin bloques nuevos.
    snap(e, now + AVM_METER_ACTIVE_NS + MS, s, d);
    CHECK(!s[0].active && s[0].peak == 0, "al detener el audio, la fuente pasa a inactiva y pico 0");
    CHECK(!d[AVM_DST_LINK_BASE].active && d[AVM_DST_LINK_BASE].peak == 0, "y el destino también");

    // Un pico fuerte seguido de señal baja no queda fijo: desaparece en ≤ 2 ventanas.
    now += 10 * SEC;
    float quiet[8] = { 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f };
    t = write_const(e, AVM_SRC_GROUP_SMAART, t + 1000, 1, loud, &now);
    for (int i = 0; i < 30; i++) t = write_const(e, AVM_SRC_GROUP_SMAART, t, 1, quiet, &now);  // 150 ms
    snap(e, now, s, d);
    CHECK(s[0].peak == 0.1f, "tras 150 ms de señal baja ya no se muestra el pico antiguo (%.3f)", s[0].peak);

    // Dos lecturas seguidas devuelven lo mismo: los lectores no se roban los picos.
    avm_meter_reading s2[AVM_NUM_SOURCES], d2[AVM_NUM_DESTS];
    snap(e, now, s2, d2);
    CHECK(s2[0].peak == s[0].peak && s2[0].active == s[0].active, "leer no consume ni borra el medidor");
    free(e);
}

static void test_measurement_does_not_change_audio(void)
{
    // Dos motores idénticos: uno con medición y otro sin ella. Lo transportado debe ser idéntico bit a bit.
    avm_engine* a = new_engine();
    avm_engine* b = new_engine();
    avm_matrix m;
    avm_matrix_default(&m);
    m.cell[AVM_DST_LINK_BASE + 7][AVM_SRC_MUSIC_BASE] = 1;  // incluir una suma
    avm_engine_set_matrix(a, &m);
    avm_engine_set_matrix(b, &m);
    float buf[BLOCK * 8], mus[BLOCK * 2];
    uint64_t now = 80 * SEC;
    int64_t t = 0;
    for (int blk = 0; blk < 16; blk++) {
        for (int i = 0; i < BLOCK * 8; i++) buf[i] = sinf((float)(t * 8 + i) * 0.013f) * 1.3f;  // incluye > 1,0
        for (int i = 0; i < BLOCK * 2; i++) mus[i] = cosf((float)(t * 2 + i) * 0.007f) * 0.4f;
        avm_engine_write_metered(a, AVM_SRC_GROUP_SMAART, t, BLOCK, buf, AVM_FMT_FLOAT32, 8, now);
        avm_engine_write(b, AVM_SRC_GROUP_SMAART, t, BLOCK, buf, AVM_FMT_FLOAT32, 8);
        avm_engine_write_metered(a, AVM_SRC_GROUP_MUSIC, t, BLOCK, mus, AVM_FMT_FLOAT32, 2, now);
        avm_engine_write(b, AVM_SRC_GROUP_MUSIC, t, BLOCK, mus, AVM_FMT_FLOAT32, 2);
        t += BLOCK;
        now += 5 * MS;
    }
    int same_ring = memcmp(a->ring, b->ring, sizeof(a->ring)) == 0;
    CHECK(same_ring, "medir al escribir no cambia ni una muestra del buffer");
    const avm_format fmts[] = { AVM_FMT_FLOAT32, AVM_FMT_SINT32, AVM_FMT_SINT24, AVM_FMT_SINT16 };
    int same_out = 1;
    for (int f = 0; f < 4; f++) {
        uint8_t oa[BLOCK * 8 * 4], ob[BLOCK * 8 * 4];
        for (int g = 0; g < AVM_DST_GROUP_COUNT; g++) {
            avm_engine_read_metered(a, (avm_dst_group)g, 4 * BLOCK, BLOCK, oa, fmts[f], 8, now);
            avm_engine_read(b, (avm_dst_group)g, 4 * BLOCK, BLOCK, ob, fmts[f], 8);
            if (memcmp(oa, ob, (size_t)BLOCK * 8 * avm_format_bytes_per_sample(fmts[f])) != 0) same_out = 0;
        }
    }
    CHECK(same_out, "medir al leer no cambia la salida, en los 4 formatos");

    // Leer los medidores muchas veces (como una app abierta/cerrada) no altera el audio.
    uint8_t before[BLOCK * 8 * 4], after[BLOCK * 8 * 4];
    avm_engine_read(b, AVM_DST_GROUP_LINK, 6 * BLOCK, BLOCK, before, AVM_FMT_FLOAT32, 8);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    for (int i = 0; i < 10000; i++) avm_engine_meter_snapshot(b, now + (uint64_t)i, s, d);
    avm_engine_read(b, AVM_DST_GROUP_LINK, 6 * BLOCK, BLOCK, after, AVM_FMT_FLOAT32, 8);
    CHECK(memcmp(before, after, BLOCK * 8 * 4) == 0, "10 000 lecturas de medidores no cambian el audio");
    CHECK(memcmp(a->ring, b->ring, sizeof(a->ring)) == 0, "ni el buffer");
    free(a);
    free(b);
}

static void test_pack_unpack(void)
{
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS], s2[AVM_NUM_SOURCES], d2[AVM_NUM_DESTS];
    for (int i = 0; i < AVM_NUM_SOURCES; i++) { s[i].peak = (float)i / 20.0f; s[i].active = i % 2; s[i].clip = i % 3 == 0; }
    for (int i = 0; i < AVM_NUM_DESTS; i++) { d[i].peak = 1.5f - (float)i / 16.0f; d[i].active = i % 3; d[i].clip = i == 7; }
    uint8_t blob[AVM_METERS_BLOB_SIZE];
    avm_meters_pack(s, d, blob);
    CHECK(avm_meters_unpack(blob, sizeof(blob), s2, d2), "desempaquetado correcto");
    int ok = 1;
    for (int i = 0; i < AVM_NUM_SOURCES; i++) if (s2[i].peak != s[i].peak || s2[i].active != s[i].active || s2[i].clip != s[i].clip) ok = 0;
    for (int i = 0; i < AVM_NUM_DESTS; i++) if (d2[i].peak != d[i].peak || d2[i].active != d[i].active || d2[i].clip != d[i].clip) ok = 0;
    CHECK(ok, "ida y vuelta exacta de los 34 medidores");
    CHECK(!avm_meters_unpack(blob, sizeof(blob) - 1, s2, d2), "tamaño incorrecto: rechazado");
    blob[0] ^= 0xFF;
    CHECK(!avm_meters_unpack(blob, sizeof(blob), s2, d2), "cabecera incorrecta: rechazada");
}

typedef struct { avm_engine* e; _Atomic int stop; _Atomic long bad; _Atomic long reads; } conc_ctx;

static void* conc_writer(void* p)
{
    conc_ctx* x = (conc_ctx*)p;
    float buf[BLOCK * 8];
    uint64_t now = 100 * SEC;
    int64_t t = 0;
    for (int i = 0; i < 20000; i++) {
        float v = (i & 1) ? 0.25f : 0.5f;
        for (int k = 0; k < BLOCK * 8; k++) buf[k] = v;
        avm_engine_write_metered(x->e, AVM_SRC_GROUP_SMAART, t, BLOCK, buf, AVM_FMT_FLOAT32, 8, now);
        float out[BLOCK * 8];
        avm_engine_read_metered(x->e, AVM_DST_GROUP_LINK, t, BLOCK, out, AVM_FMT_FLOAT32, 8, now);
        t += BLOCK;
        now += 1 * MS;
    }
    atomic_store(&x->stop, 1);
    return NULL;
}

static void* conc_reader(void* p)
{
    conc_ctx* x = (conc_ctx*)p;
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    while (!atomic_load(&x->stop)) {
        avm_engine_meter_snapshot(x->e, 100 * SEC + 10 * MS, s, d);
        float v = s[0].peak;
        if (!(v == 0.0f || v == 0.25f || v == 0.5f)) atomic_fetch_add(&x->bad, 1);
        atomic_fetch_add(&x->reads, 1);
    }
    return NULL;
}

static void test_concurrency(void)
{
    conc_ctx x;
    x.e = new_engine();
    atomic_init(&x.stop, 0);
    atomic_init(&x.bad, 0);
    atomic_init(&x.reads, 0);
    pthread_t w, r;
    pthread_create(&r, NULL, conc_reader, &x);
    pthread_create(&w, NULL, conc_writer, &x);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    CHECK(atomic_load(&x.bad) == 0, "lecturas concurrentes de medidores sin valores imposibles (%ld)", atomic_load(&x.bad));
    printf("  medidores concurrentes: %ld lecturas\n", atomic_load(&x.reads));
    free(x.e);
}

int main(void)
{
    test_never_written_and_silence();
    test_known_signal();
    test_channel_separation();
    test_destinations_after_routing();
    test_destination_sum();
    test_clip();
    test_no_stale_values();
    test_measurement_does_not_change_audio();
    test_pack_unpack();
    test_concurrency();
    printf("Medidores: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
