// Pruebas del motor con las fuentes EVO 1-4 y los destinos Dante L/R
// (núcleo, sin CoreAudio): reglas de la matriz, presets intactos, lectura de
// ajustes de la versión anterior, aislamiento de los 4 canales EVO, separación
// L/R hacia Dante, volumen y MUTE (desde la primera muestra), desconexión y
// reconexión sin audio viejo, y que Smaart y LISTENTO no cambian.

#include "../Driver/src/avm_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define BLOCK 256
#define EVO(k)    (AVM_SRC_EVO_BASE + (k))
#define DANTE(k)  (AVM_DST_DANTE_BASE + (k))

static avm_engine* new_engine(void)
{
    avm_engine* e = (avm_engine*)malloc(sizeof(avm_engine));
    avm_engine_init(e);
    return e;
}

// Valores distintos y exactos (k/64) por canal.
static float evo_val(int k)   { return (float)(k + 1) / 16.0f + 1.0f / 64.0f; }   // 0.078, 0.140, ...
static float music_val(int k) { return k == 0 ? 0.375f : -0.4375f; }

static void write_evo(avm_engine* e, int64_t t, const float v[4])
{
    float buf[BLOCK * 4];
    for (int f = 0; f < BLOCK; f++) for (int c = 0; c < 4; c++) buf[f * 4 + c] = v[c];
    avm_engine_write(e, AVM_SRC_GROUP_EVO, t, BLOCK, buf, AVM_FMT_FLOAT32, 4);
}

static void write_music(avm_engine* e, int64_t t)
{
    float buf[BLOCK * 2];
    for (int f = 0; f < BLOCK; f++) { buf[f * 2] = music_val(0); buf[f * 2 + 1] = music_val(1); }
    avm_engine_write(e, AVM_SRC_GROUP_MUSIC, t, BLOCK, buf, AVM_FMT_FLOAT32, 2);
}

static void read_all(avm_engine* e, int64_t t, float sm[BLOCK * 8], float li[BLOCK * 8], float da[BLOCK * 2])
{
    avm_engine_read(e, AVM_DST_GROUP_SMAART, t, BLOCK, sm, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, t, BLOCK, li, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_DANTE, t, BLOCK, da, AVM_FMT_FLOAT32, 2);
}

static void test_layout_and_rules(void)
{
    CHECK(AVM_NUM_SOURCES == 22 && AVM_NUM_DESTS == 18, "22 fuentes y 18 destinos");
    CHECK(avm_src_group_base(AVM_SRC_GROUP_EVO) == 18 && avm_src_group_channels(AVM_SRC_GROUP_EVO) == 4, "EVO 1-4 = fuentes 19-22");
    CHECK(avm_dst_group_base(AVM_DST_GROUP_DANTE) == 16 && avm_dst_group_channels(AVM_DST_GROUP_DANTE) == 2, "Dante L/R = destinos 17-18");
    int ok = 1;
    for (int k = 0; k < 4; k++) if (avm_src_group_of(EVO(k)) != AVM_SRC_GROUP_EVO) ok = 0;
    if (avm_src_group_of(AVM_SRC_MUSIC_BASE + 1) != AVM_SRC_GROUP_MUSIC) ok = 0;
    CHECK(ok, "grupo de cada fuente nueva");

    int evo_any = 1, to_dante = 1;
    for (int d = 0; d < AVM_NUM_DESTS; d++) for (int k = 0; k < 4; k++) if (avm_route_allowed(d, EVO(k)) != AVM_ROUTE_OK) evo_any = 0;
    for (int s = 0; s < AVM_NUM_SOURCES; s++) for (int k = 0; k < 2; k++) if (avm_route_allowed(DANTE(k), s) != AVM_ROUTE_OK) to_dante = 0;
    CHECK(evo_any, "cada entrada EVO se puede conectar a cualquier destino (Smaart, LISTENTO y Dante)");
    CHECK(to_dante, "cualquier fuente se puede conectar a Dante L/R (Dante no vuelve a entrar)");
    CHECK(avm_route_allowed(AVM_DST_LINK_BASE + 2, AVM_SRC_LINK_BASE + 2) == AVM_ROUTE_FORBIDDEN_FEEDBACK
          && avm_route_allowed(AVM_DST_LINK_BASE + 7, AVM_SRC_LINK_BASE + 0) == AVM_ROUTE_FORBIDDEN_FEEDBACK,
          "se mantiene el bloqueo LISTENTO retorno -> LISTENTO envío");
    CHECK(avm_route_allowed(AVM_NUM_DESTS, 0) == AVM_ROUTE_OUT_OF_RANGE && avm_route_allowed(0, AVM_NUM_SOURCES) == AVM_ROUTE_OUT_OF_RANGE, "fuera de rango");
}

// Presets: las rutas de Smaart y LISTENTO no cambian y no tocan EVO ni Dante.
static void test_presets_unchanged(void)
{
    avm_matrix m; avm_matrix_default(&m);
    int ok = 1;
    for (int d = 0; d < AVM_NUM_DESTS; d++)
        for (int s = 0; s < AVM_NUM_SOURCES; s++) {
            int want = 0;
            if (d >= AVM_DST_LINK_BASE && d < AVM_DST_LINK_BASE + 8 && s == d - AVM_DST_LINK_BASE) want = 1;      // Salida N -> Envío N
            if (d < 7 && s == AVM_SRC_LINK_BASE + d) want = 1;                                                       // Retorno N -> Entrada N (1-7)
            if (d == AVM_DST_LOCAL_REFERENCE && s == AVM_SRC_GENERATOR) want = 1;                                   // Salida 1 -> Entrada 8
            if (m.cell[d][s] != want) ok = 0;
        }
    CHECK(ok, "preset de medición idéntico (incluida la referencia local Salida 1 -> Entrada 8) y sin rutas EVO ni Dante");
    avm_matrix_direct(&m);
    int none = 1;
    for (int d = 0; d < AVM_NUM_DESTS; d++) for (int k = 0; k < 4; k++) if (m.cell[d][EVO(k)]) none = 0;
    for (int s = 0; s < AVM_NUM_SOURCES; s++) for (int k = 0; k < 2; k++) if (m.cell[DANTE(k)][s]) none = 0;
    CHECK(none, "preset directo sin rutas EVO ni Dante");

    // Un preset conserva las rutas hacia Dante que ya había.
    avm_matrix cur; avm_matrix_clear(&cur);
    cur.cell[DANTE(0)][AVM_SRC_MUSIC_BASE] = 1; cur.cell[DANTE(1)][AVM_SRC_MUSIC_BASE + 1] = 1;
    avm_matrix_default(&m);
    avm_matrix_copy_dst_group(&m, &cur, AVM_DST_GROUP_DANTE);
    CHECK(m.cell[DANTE(0)][AVM_SRC_MUSIC_BASE] && m.cell[DANTE(1)][AVM_SRC_MUSIC_BASE + 1] && m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR]
          && avm_matrix_is_valid(&m), "preset de medición + rutas Dante conservadas");
}

static void test_legacy_settings(void)
{
    // Matriz guardada por la versión anterior: 16 x 18 bytes.
    uint8_t old[AVM_LEGACY_NUM_DESTS * AVM_LEGACY_NUM_SOURCES];
    memset(old, 0, sizeof(old));
    old[AVM_DST_LOCAL_REFERENCE * AVM_LEGACY_NUM_SOURCES + AVM_SRC_GENERATOR] = 1;
    old[(AVM_DST_LINK_BASE + 3) * AVM_LEGACY_NUM_SOURCES + 3] = 1;
    old[15 * AVM_LEGACY_NUM_SOURCES + 17] = 1;   // Música R -> Envío 8
    avm_matrix m;
    CHECK(avm_matrix_from_bytes(old, sizeof(old), &m), "matriz guardada antigua (288 bytes) aceptada");
    int ok = m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR] && m.cell[AVM_DST_LINK_BASE + 3][3] && m.cell[15][17];
    int n = 0;
    for (int d = 0; d < AVM_NUM_DESTS; d++) for (int s = 0; s < AVM_NUM_SOURCES; s++) n += m.cell[d][s];
    CHECK(ok && n == 3, "cada cruce antiguo en su sitio; EVO y Dante vacíos");
    uint8_t cur[sizeof(m.cell)];
    memcpy(cur, m.cell, sizeof(cur));
    avm_matrix m2;
    CHECK(avm_matrix_from_bytes(cur, sizeof(cur), &m2) && !memcmp(m2.cell, m.cell, sizeof(cur)), "matriz actual (396 bytes)");
    CHECK(!avm_matrix_from_bytes(cur, 100, &m2) && !avm_matrix_from_bytes(NULL, sizeof(cur), &m2), "otros tamaños rechazados");

    // Mixer guardado por la versión anterior (18 fuentes, 16 destinos).
    uint8_t blob[AVM_MIXER_LEGACY_BLOB_SIZE];
    memset(blob, 0, sizeof(blob));
    memcpy(blob, "AVX1", 4);
    uint32_t ns = AVM_LEGACY_NUM_SOURCES, nd = AVM_LEGACY_NUM_DESTS;
    memcpy(blob + 4, &ns, 4); memcpy(blob + 8, &nd, 4);
    for (int i = 0; i < 34; i++) { float g = 1.0f; memcpy(blob + 16 + 8 * i, &g, 4); }
    float half = 0.5f;
    memcpy(blob + 16 + 8 * 17, &half, 4);         // Música R a −6 dB
    blob[16 + 8 * (18 + 7) + 4] = 1;              // Entrada 8 en MUTE
    avm_mixer mx;
    CHECK(avm_mixer_unpack(blob, sizeof(blob), &mx), "mixer guardado antiguo aceptado");
    CHECK(mx.src_gain[17] == 0.5f && mx.dst_mute[7] == 1, "ajustes antiguos conservados");
    int fresh = 1;
    for (int k = 0; k < 4; k++) if (mx.src_gain[EVO(k)] != 1.0f || mx.src_mute[EVO(k)]) fresh = 0;
    for (int k = 0; k < 2; k++) if (mx.dst_gain[DANTE(k)] != 1.0f || mx.dst_mute[DANTE(k)]) fresh = 0;
    CHECK(fresh, "EVO y Dante empiezan a 0 dB sin MUTE");
    CHECK(!avm_mixer_unpack(blob, sizeof(blob) - 8, &mx), "tamaño antiguo incompleto rechazado");
}

// Las cuatro entradas EVO llegan cada una solo a su destino, sin mezclarse.
static void test_evo_isolation(void)
{
    avm_engine* e = new_engine();
    avm_matrix m; avm_matrix_default(&m);       // las rutas de medición siguen
    for (int k = 0; k < 4; k++) {
        m.cell[AVM_DST_SMAART_BASE + 0][EVO(k)] = 0;
    }
    // EVO 1 -> Envío 6, EVO 2 -> Envío 7, EVO 3 -> Dante L, EVO 4 -> Dante R.
    for (int k = 0; k < 2; k++) {
        m.cell[AVM_DST_LINK_BASE + 5 + k][AVM_SRC_SMAART_BASE + 5 + k] = 0;
        m.cell[AVM_DST_LINK_BASE + 5 + k][EVO(k)] = 1;
    }
    m.cell[DANTE(0)][EVO(2)] = 1;
    m.cell[DANTE(1)][EVO(3)] = 1;
    avm_engine_set_matrix(e, &m);
    avm_engine_reset(e);
    float v[4] = { evo_val(0), evo_val(1), evo_val(2), evo_val(3) };
    float sm[BLOCK * 8], li[BLOCK * 8], da[BLOCK * 2];
    write_evo(e, 0, v);
    read_all(e, 0, sm, li, da);
    int ok = 1, sep = 1;
    for (int f = 0; f < BLOCK; f++) {
        if (li[f * 8 + 5] != v[0] || li[f * 8 + 6] != v[1] || da[f * 2] != v[2] || da[f * 2 + 1] != v[3]) ok = 0;
        for (int c = 0; c < 8; c++) if (sm[f * 8 + c] != 0.0f) sep = 0;           // EVO no conectada a Smaart
        for (int c = 0; c < 5; c++) if (li[f * 8 + c] != 0.0f) sep = 0;
        if (li[f * 8 + 7] != 0.0f) sep = 0;
    }
    CHECK(ok, "EVO 1-4 llegan exactas, cada una a su destino (Envío 6, Envío 7, Dante L, Dante R)");
    CHECK(sep, "ningún otro destino recibe EVO (sin mezcla automática)");

    // Cada EVO a la vez a Smaart Entrada k: cuatro canales distintos y separados.
    avm_matrix_clear(&m);
    for (int k = 0; k < 4; k++) m.cell[AVM_DST_SMAART_BASE + k][EVO(k)] = 1;
    avm_engine_set_matrix(e, &m);
    write_evo(e, BLOCK, v);
    read_all(e, BLOCK, sm, li, da);
    int four = 1;
    for (int f = 0; f < BLOCK; f++) {
        for (int k = 0; k < 4; k++) if (sm[f * 8 + k] != v[k]) four = 0;
        for (int c = 4; c < 8; c++) if (sm[f * 8 + c] != 0.0f) four = 0;
    }
    CHECK(four, "EVO 1-4 -> Smaart Entradas 1-4: cuatro canales exactos y separados");

    // Suma explícita (solo si se pide): EVO 1 + EVO 2 -> Smaart Entrada 5.
    m.cell[AVM_DST_SMAART_BASE + 4][EVO(0)] = 1;
    m.cell[AVM_DST_SMAART_BASE + 4][EVO(1)] = 1;
    avm_engine_set_matrix(e, &m);
    write_evo(e, 2 * BLOCK, v);
    read_all(e, 2 * BLOCK, sm, li, da);
    CHECK(sm[4] == v[0] + v[1] && sm[0] == v[0] && sm[1] == v[1], "la suma solo ocurre si se conectan dos casillas");
    free(e);
}

static void test_dante_lr(void)
{
    avm_engine* e = new_engine();
    avm_matrix m; avm_matrix_default(&m);
    m.cell[DANTE(0)][AVM_SRC_MUSIC_BASE] = 1;
    m.cell[DANTE(1)][AVM_SRC_MUSIC_BASE + 1] = 1;
    avm_engine_set_matrix(e, &m);
    avm_engine_reset(e);
    float sm[BLOCK * 8], li[BLOCK * 8], da[BLOCK * 2];
    write_music(e, 0);
    read_all(e, 0, sm, li, da);
    int ok = 1;
    for (int f = 0; f < BLOCK; f++) if (da[f * 2] != music_val(0) || da[f * 2 + 1] != music_val(1)) ok = 0;
    CHECK(ok, "Música L -> Dante L y Música R -> Dante R, exactos y separados");
    int nothing = 1;
    for (int i = 0; i < BLOCK * 8; i++) if (sm[i] != 0.0f || li[i] != 0.0f) nothing = 0;
    CHECK(nothing, "la música no llega a Smaart ni a LISTENTO si no se conecta");

    // Volumen y MUTE: Música L a −6 dB (fuente) y Dante R en MUTE (destino).
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_gain[AVM_SRC_MUSIC_BASE] = 0.5f;
    mx.dst_mute[DANTE(1)] = 1;
    avm_engine_set_mixer(e, &mx);
    for (int b = 1; b < 6; b++) { write_music(e, b * BLOCK); read_all(e, b * BLOCK, sm, li, da); }
    CHECK(da[0] == music_val(0) * 0.5f && da[1] == 0.0f && !signbit(da[1]), "volumen de Música L y MUTE de Dante R (tras la rampa)");

    // MUTE desde la primera muestra: Dante empieza a leer con Dante L ya en MUTE.
    avm_engine* e2 = new_engine();
    avm_engine_set_matrix(e2, &m);
    avm_engine_reset(e2);
    for (int b = 0; b < 4; b++) write_music(e2, b * BLOCK);           // la música ya suena; Dante aún no lee
    avm_mixer_default(&mx);
    mx.dst_mute[DANTE(0)] = 1;
    mx.src_gain[AVM_SRC_MUSIC_BASE + 1] = 0.25f;
    avm_engine_set_mixer(e2, &mx);
    write_music(e2, 4 * BLOCK);
    avm_engine_read(e2, AVM_DST_GROUP_DANTE, 4 * BLOCK, BLOCK, da, AVM_FMT_FLOAT32, 2);
    int first = 1;
    for (int f = 0; f < BLOCK; f++) if (da[f * 2] != 0.0f) first = 0;
    CHECK(first, "Dante empieza con L en MUTE: silencio desde la primera muestra (primera %g)", (double)da[0]);
    CHECK(da[1] != music_val(1) && fabsf(da[1]) <= fabsf(music_val(1)), "el volumen de Música R ya en rampa no amplifica");
    free(e); free(e2);
}

// EVO: volumen, MUTE desde la primera muestra, desconexión y reconexión.
static void test_evo_mixer_and_reconnect(void)
{
    avm_engine* e = new_engine();
    avm_matrix m; avm_matrix_clear(&m);
    for (int k = 0; k < 4; k++) m.cell[AVM_DST_SMAART_BASE + k][EVO(k)] = 1;
    m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR] = 1;   // referencia local activa a la vez
    avm_engine_set_matrix(e, &m);
    avm_engine_reset(e);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.src_mute[EVO(1)] = 1;          // EVO 2 en MUTE antes de conectar la EVO
    mx.src_gain[EVO(2)] = 0.5f;       // EVO 3 a −6 dB
    avm_engine_set_mixer(e, &mx);
    float smaart_out[BLOCK * 8], sm[BLOCK * 8];
    for (int i = 0; i < BLOCK * 8; i++) smaart_out[i] = 0.25f;
    // Smaart ya en marcha; la EVO aún no.
    for (int b = 0; b < 4; b++) {
        avm_engine_write(e, AVM_SRC_GROUP_SMAART, b * BLOCK, BLOCK, smaart_out, AVM_FMT_FLOAT32, 8);
        avm_engine_read(e, AVM_DST_GROUP_SMAART, b * BLOCK, BLOCK, sm, AVM_FMT_FLOAT32, 8);
    }
    float v[4] = { evo_val(0), evo_val(1), evo_val(2), evo_val(3) };
    int64_t t = 4 * BLOCK;
    avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, smaart_out, AVM_FMT_FLOAT32, 8);
    write_evo(e, t, v);                                   // la EVO se conecta
    avm_engine_read(e, AVM_DST_GROUP_SMAART, t, BLOCK, sm, AVM_FMT_FLOAT32, 8);
    int ok = 1;
    for (int f = 0; f < BLOCK; f++)
        if (sm[f * 8] != v[0] || sm[f * 8 + 1] != 0.0f || sm[f * 8 + 2] != v[2] * 0.5f || sm[f * 8 + 3] != v[3] || sm[f * 8 + 7] != 0.25f) ok = 0;
    CHECK(ok, "al conectar la EVO: MUTE y −6 dB desde la primera muestra; la referencia local sigue exacta");

    // Desconexión: la EVO deja de escribir. Lo que se lee después es silencio, nunca audio viejo.
    t += BLOCK;
    int stale = 0, blocks = 0;
    for (int b = 0; b < 300; b++, t += BLOCK) {      // 300 bloques: más que la ventana de validez
        avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, smaart_out, AVM_FMT_FLOAT32, 8);
        avm_engine_read(e, AVM_DST_GROUP_SMAART, t, BLOCK, sm, AVM_FMT_FLOAT32, 8);
        for (int f = 0; f < BLOCK; f++) for (int k = 0; k < 4; k++) if (sm[f * 8 + k] != 0.0f) stale++;
        blocks++;
    }
    CHECK(stale == 0, "EVO desconectada: sus destinos en silencio (%d muestras no nulas en %d bloques)", stale, blocks);

    // Reconexión mucho después: solo audio nuevo, con el mixer vigente desde la primera muestra.
    mx.src_mute[EVO(1)] = 0;
    mx.src_mute[EVO(0)] = 1;
    avm_engine_set_mixer(e, &mx);
    float v2[4] = { -0.125f, -0.25f, -0.375f, -0.5f };
    avm_engine_write(e, AVM_SRC_GROUP_SMAART, t, BLOCK, smaart_out, AVM_FMT_FLOAT32, 8);
    write_evo(e, t, v2);
    avm_engine_read(e, AVM_DST_GROUP_SMAART, t, BLOCK, sm, AVM_FMT_FLOAT32, 8);
    int fresh = 1;
    for (int f = 0; f < BLOCK; f++)
        if (sm[f * 8] != 0.0f || sm[f * 8 + 1] != v2[1] || sm[f * 8 + 2] != v2[2] * 0.5f || sm[f * 8 + 3] != v2[3]) fresh = 0;
    CHECK(fresh, "al reconectar: solo valores nuevos y el mixer vigente desde la primera muestra");

    // Leer un instante anterior a la reconexión (dentro del hueco) da silencio.
    avm_engine_read(e, AVM_DST_GROUP_SMAART, t - BLOCK, BLOCK, sm, AVM_FMT_FLOAT32, 8);
    int gap = 1;
    for (int f = 0; f < BLOCK; f++) for (int k = 0; k < 4; k++) if (sm[f * 8 + k] != 0.0f) gap = 0;
    CHECK(gap, "el hueco de la desconexión se lee como silencio");
    free(e);
}

// Medidores de las fuentes EVO y de los destinos Dante.
static void test_meters(void)
{
    avm_engine* e = new_engine();
    avm_matrix m; avm_matrix_clear(&m);
    m.cell[DANTE(0)][EVO(0)] = 1;
    m.cell[DANTE(1)][EVO(3)] = 1;
    avm_engine_set_matrix(e, &m);
    avm_mixer mx; avm_mixer_default(&mx);
    mx.dst_gain[DANTE(1)] = 0.5f;
    avm_engine_set_mixer(e, &mx);
    avm_engine_reset(e);
    float v[4] = { evo_val(0), evo_val(1), evo_val(2), evo_val(3) };
    float buf[BLOCK * 4], da[BLOCK * 2];
    for (int f = 0; f < BLOCK; f++) for (int c = 0; c < 4; c++) buf[f * 4 + c] = v[c];
    avm_engine_write_metered(e, AVM_SRC_GROUP_EVO, 0, BLOCK, buf, AVM_FMT_FLOAT32, 4, 1000000000ull);
    avm_engine_read_metered(e, AVM_DST_GROUP_DANTE, 0, BLOCK, da, AVM_FMT_FLOAT32, 2, 1000000000ull);
    avm_meter_reading s[AVM_NUM_SOURCES], d[AVM_NUM_DESTS];
    avm_engine_meter_snapshot(e, 1000000000ull, s, d);
    int ok = 1;
    for (int k = 0; k < 4; k++) if (!s[EVO(k)].active || s[EVO(k)].peak != v[k]) ok = 0;
    CHECK(ok, "medidores de EVO 1-4 con su nivel");
    CHECK(d[DANTE(0)].peak == v[0] && d[DANTE(1)].peak == v[3] * 0.5f, "medidores de Dante L/R tras el volumen");
    uint8_t blob[AVM_METERS_BLOB_SIZE];
    avm_meters_pack(s, d, blob);
    avm_meter_reading s2[AVM_NUM_SOURCES], d2[AVM_NUM_DESTS];
    CHECK(avm_meters_unpack(blob, sizeof(blob), s2, d2) && s2[EVO(3)].peak == v[3] && d2[DANTE(1)].peak == v[3] * 0.5f, "los medidores nuevos viajan en la propiedad 'metr'");
    free(e);
}

// Con EVO y Dante en uso, Smaart y LISTENTO reciben exactamente lo mismo que sin ellos.
static void test_smaart_listento_untouched(void)
{
    avm_engine* a = new_engine();
    avm_engine* b = new_engine();
    avm_matrix m; avm_matrix_default(&m);
    avm_engine_set_matrix(a, &m);
    m.cell[DANTE(0)][AVM_SRC_MUSIC_BASE] = 1; m.cell[DANTE(1)][AVM_SRC_MUSIC_BASE + 1] = 1;
    m.cell[DANTE(0)][EVO(0)] = 1;
    avm_engine_set_matrix(b, &m);
    avm_engine_reset(a); avm_engine_reset(b);
    float sm_in[BLOCK * 8], li_in[BLOCK * 8], v[4] = { 0.5f, 0.25f, 0.125f, 0.0625f };
    for (int i = 0; i < BLOCK * 8; i++) { sm_in[i] = (float)(i % 97) / 128.0f; li_in[i] = -(float)(i % 89) / 128.0f; }
    int same = 1;
    for (int blk = 0; blk < 20; blk++) {
        int64_t t = blk * BLOCK;
        float sa[BLOCK * 8], la[BLOCK * 8], sb[BLOCK * 8], lb[BLOCK * 8], da[BLOCK * 2];
        avm_engine* es[2] = { a, b };
        for (int k = 0; k < 2; k++) {
            avm_engine_write(es[k], AVM_SRC_GROUP_SMAART, t, BLOCK, sm_in, AVM_FMT_FLOAT32, 8);
            avm_engine_write(es[k], AVM_SRC_GROUP_LINK, t, BLOCK, li_in, AVM_FMT_FLOAT32, 8);
        }
        write_music(b, t);
        write_evo(b, t, v);
        avm_engine_read(b, AVM_DST_GROUP_DANTE, t, BLOCK, da, AVM_FMT_FLOAT32, 2);
        avm_engine_read(a, AVM_DST_GROUP_SMAART, t, BLOCK, sa, AVM_FMT_FLOAT32, 8);
        avm_engine_read(a, AVM_DST_GROUP_LINK, t, BLOCK, la, AVM_FMT_FLOAT32, 8);
        avm_engine_read(b, AVM_DST_GROUP_SMAART, t, BLOCK, sb, AVM_FMT_FLOAT32, 8);
        avm_engine_read(b, AVM_DST_GROUP_LINK, t, BLOCK, lb, AVM_FMT_FLOAT32, 8);
        if (memcmp(sa, sb, sizeof(sa)) || memcmp(la, lb, sizeof(la))) same = 0;
    }
    CHECK(same, "Smaart y LISTENTO idénticos bit a bit con la música y la EVO yendo a Dante");
    free(a); free(b);
}

int main(void)
{
    test_layout_and_rules();
    test_presets_unchanged();
    test_legacy_settings();
    test_evo_isolation();
    test_dante_lr();
    test_evo_mixer_and_reconnect();
    test_meters();
    test_smaart_listento_untouched();
    printf("EVO y Dante (motor): %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
