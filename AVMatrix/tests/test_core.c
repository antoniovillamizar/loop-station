// Pruebas unitarias del núcleo de AV Matrix (sin CoreAudio).
// Compilar y ejecutar:  make -C AVMatrix test

#include "../Driver/src/avm_core.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static avm_engine* new_engine(void)
{
    avm_engine* e = (avm_engine*)malloc(sizeof(avm_engine));
    avm_engine_init(e);
    return e;
}

// Valor distinto y exacto para cada fuente: identifica de dónde viene cada muestra.
static float src_value(int src) { return (float)(src + 1) / 64.0f; }

static void write_constant_sources(avm_engine* e, int64_t t0, uint32_t frames)
{
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        int base = avm_src_group_base((avm_src_group)g);
        int nch = avm_src_group_channels((avm_src_group)g);
        float* buf = (float*)calloc((size_t)frames * nch, sizeof(float));
        for (uint32_t f = 0; f < frames; f++)
            for (int c = 0; c < nch; c++)
                buf[f * nch + c] = src_value(base + c);
        avm_engine_write(e, (avm_src_group)g, t0, frames, buf, AVM_FMT_FLOAT32, (uint32_t)nch);
        free(buf);
    }
}

static void test_rules(void)
{
    avm_matrix m;
    avm_matrix_default(&m);
    CHECK(avm_matrix_is_valid(&m), "el preset predeterminado (medición) debe ser válido");

    for (int d = AVM_DST_LINK_BASE; d < AVM_DST_LINK_BASE + 8; d++)
        for (int s = AVM_SRC_LINK_BASE; s < AVM_SRC_LINK_BASE + 8; s++)
            CHECK(avm_route_allowed(d, s) == AVM_ROUTE_FORBIDDEN_FEEDBACK,
                  "retorno LISTENTO %d -> envío LISTENTO %d debe estar prohibido", s, d);

    CHECK(avm_route_allowed(AVM_DST_SMAART_BASE, AVM_SRC_SMAART_BASE) == AVM_ROUTE_OK,
          "Smaart salida -> Smaart entrada (referencia en bucle) debe estar permitido");
    CHECK(avm_route_allowed(AVM_DST_LINK_BASE, AVM_SRC_MUSIC_BASE) == AVM_ROUTE_OK,
          "Música -> envío LISTENTO debe estar permitido");
    CHECK(avm_route_allowed(AVM_NUM_DESTS, 0) == AVM_ROUTE_OUT_OF_RANGE, "rango destino");
    CHECK(avm_route_allowed(0, AVM_NUM_SOURCES) == AVM_ROUTE_OUT_OF_RANGE, "rango fuente");

    m.cell[AVM_DST_LINK_BASE + 2][AVM_SRC_LINK_BASE + 2] = 1;
    CHECK(!avm_matrix_is_valid(&m), "una matriz con lazo debe ser inválida");

    avm_matrix_default(&m);
    m.cell[0][0] = 2;
    CHECK(!avm_matrix_is_valid(&m), "solo se admiten valores 0/1");
}

static void test_isolation_default(void)
{
    avm_engine* e = new_engine();
    const uint32_t N = 512;
    write_constant_sources(e, 0, 4096);

    float smaart_in[512 * 8], link_in[512 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 1000, N, smaart_in, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, 1000, N, link_in, AVM_FMT_FLOAT32, 8);

    int ok_s = 1, ok_ref = 1, ok_l = 1;
    for (uint32_t f = 0; f < N; f++)
        for (int c = 0; c < 8; c++) {
            if (c < 7 && smaart_in[f * 8 + c] != src_value(AVM_SRC_LINK_BASE + c)) ok_s = 0;
            if (c == 7 && smaart_in[f * 8 + c] != src_value(AVM_SRC_GENERATOR)) ok_ref = 0;
            if (link_in[f * 8 + c] != src_value(AVM_SRC_SMAART_BASE + c)) ok_l = 0;
        }
    CHECK(ok_s, "preset de medición: entrada Smaart N (1-7) contiene exactamente el retorno LISTENTO N");
    CHECK(ok_ref, "preset de medición: Entrada 8 (referencia local) = solo Smaart Salida 1, sin el retorno 8");
    CHECK(ok_l, "preset de medición: envío LISTENTO N contiene exactamente la salida Smaart N");
    free(e);
}

static void test_measurement_preset_cells(void)
{
    avm_matrix m;
    avm_matrix_default(&m);
    CHECK(avm_matrix_is_valid(&m), "el preset de medición es válido");
    CHECK(m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR] == 1, "Smaart Salida 1 -> Smaart Entrada 8");
    CHECK(m.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_LINK_BASE + 7] == 0, "el retorno 8 de LISTENTO no entra en la referencia local");
    int n = 0;
    for (int s = 0; s < AVM_NUM_SOURCES; s++) n += m.cell[AVM_DST_LOCAL_REFERENCE][s];
    CHECK(n == 1, "la referencia local tiene una sola fuente (%d)", n);
    for (int s = AVM_SRC_MUSIC_BASE; s < AVM_NUM_SOURCES; s++)
        for (int d = 0; d < AVM_NUM_DESTS; d++)
            if (m.cell[d][s]) { CHECK(0, "la música no se conecta en el preset"); return; }

    avm_matrix direct;
    avm_matrix_direct(&direct);
    CHECK(avm_matrix_is_valid(&direct), "el preset directo es válido");
    CHECK(direct.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_LINK_BASE + 7] == 1 && direct.cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR] == 0,
          "el preset directo lleva el retorno 8 a la entrada 8, sin referencia");
}

static void test_clear_is_silence(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    avm_engine_set_matrix(e, &m);
    write_constant_sources(e, 0, 2048);
    float buf[256 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 100, 256, buf, AVM_FMT_FLOAT32, 8);
    int silent = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(silent, "con la matriz vacía no debe pasar ninguna señal");
    free(e);
}

static void test_single_route_and_sum(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    // Referencia en bucle: Smaart salida 1 -> Smaart entrada 1.
    m.cell[AVM_DST_SMAART_BASE + 0][AVM_SRC_SMAART_BASE + 0] = 1;
    // Música L+R -> envío LISTENTO 7 (suma intencionada).
    m.cell[AVM_DST_LINK_BASE + 6][AVM_SRC_MUSIC_BASE + 0] = 1;
    m.cell[AVM_DST_LINK_BASE + 6][AVM_SRC_MUSIC_BASE + 1] = 1;
    CHECK(avm_matrix_is_valid(&m), "matriz de prueba válida");
    avm_engine_set_matrix(e, &m);
    write_constant_sources(e, 0, 2048);

    float s[64 * 8], l[64 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 500, 64, s, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, 500, 64, l, AVM_FMT_FLOAT32, 8);
    int ok = 1;
    for (int f = 0; f < 64; f++)
        for (int c = 0; c < 8; c++) {
            float exp_s = (c == 0) ? src_value(AVM_SRC_SMAART_BASE) : 0.0f;
            float exp_l = (c == 6) ? src_value(AVM_SRC_MUSIC_BASE) + src_value(AVM_SRC_MUSIC_BASE + 1) : 0.0f;
            if (s[f * 8 + c] != exp_s || l[f * 8 + c] != exp_l) ok = 0;
        }
    CHECK(ok, "ruta única y suma explícita deben comportarse exactamente como la matriz indica");
    free(e);
}

static void test_forbidden_ignored_even_if_forced(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    m.cell[AVM_DST_LINK_BASE][AVM_SRC_LINK_BASE] = 1;  // se fuerza un lazo sin validar
    avm_engine_set_matrix(e, &m);
    write_constant_sources(e, 0, 2048);
    float l[64 * 8];
    avm_engine_read(e, AVM_DST_GROUP_LINK, 100, 64, l, AVM_FMT_FLOAT32, 8);
    int silent = 1;
    for (int i = 0; i < 64 * 8; i++) if (l[i] != 0.0f) silent = 0;
    CHECK(silent, "el motor nunca debe ejecutar un cruce prohibido, aunque llegue a la matriz");
    free(e);
}

static void test_stale_data(void)
{
    avm_engine* e = new_engine();
    avm_matrix direct;
    avm_matrix_direct(&direct);  // las 8 entradas de Smaart reciben los 8 retornos
    avm_engine_set_matrix(e, &direct);
    write_constant_sources(e, 0, 512);
    float buf[256 * 8];

    // Más allá de lo escrito: silencio, nunca datos del buffer.
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 1000, 256, buf, AVM_FMT_FLOAT32, 8);
    int silent = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(silent, "sin datos nuevos de la fuente, el destino debe recibir silencio");

    // Escritura de solo LISTENTO tras un hueco: el hueco debe leerse como silencio.
    float lbuf[512 * 8];
    for (int i = 0; i < 512 * 8; i++) lbuf[i] = 0.5f;
    avm_engine_write(e, AVM_SRC_GROUP_LINK, 5000, 512, lbuf, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 600, 256, buf, AVM_FMT_FLOAT32, 8);
    silent = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(silent, "un hueco en el envío debe leerse como silencio");

    avm_engine_read(e, AVM_DST_GROUP_SMAART, 5100, 256, buf, AVM_FMT_FLOAT32, 8);
    int ok = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.5f) ok = 0;
    CHECK(ok, "los datos nuevos tras el hueco deben llegar intactos");

    // Datos más antiguos que la ventana válida: silencio.
    for (int i = 0; i < 512 * 8; i++) lbuf[i] = 0.25f;
    for (int64_t t = 5512; t < 5512 + AVM_VALID_WINDOW + 4096; t += 512)
        avm_engine_write(e, AVM_SRC_GROUP_LINK, t, 512, lbuf, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 5100, 256, buf, AVM_FMT_FLOAT32, 8);
    silent = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(silent, "datos fuera de la ventana válida no deben reproducirse");

    // Tras reset no queda nada.
    avm_engine_reset(e);
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 0, 256, buf, AVM_FMT_FLOAT32, 8);
    silent = 1;
    for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(silent, "tras reiniciar el motor no debe quedar audio anterior");
    free(e);
}

static void test_formats(void)
{
    const avm_format fmts[] = { AVM_FMT_FLOAT32, AVM_FMT_SINT32, AVM_FMT_SINT24, AVM_FMT_SINT16 };
    const char* names[] = { "Float32", "SInt32", "SInt24", "SInt16" };
    const float vals[] = { 0.0f, 0.5f, -0.5f, 0.123456f, -0.999f, 0.25f };
    for (int fi = 0; fi < 4; fi++) {
        uint8_t b[4];
        double tol = (fmts[fi] == AVM_FMT_FLOAT32) ? 0.0 : 1.0 / pow(2.0, avm_format_bits(fmts[fi]) - 1);
        if (fmts[fi] == AVM_FMT_SINT32) tol = 1e-7;  // limitado por la precisión de float
        int ok = 1;
        for (size_t i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            avm_float_to_sample(vals[i], b, fmts[fi]);
            float r = avm_sample_to_float(b, fmts[fi]);
            if (fabs((double)r - (double)vals[i]) > tol) ok = 0;
        }
        CHECK(ok, "ida y vuelta %s", names[fi]);

        avm_float_to_sample(1.5f, b, fmts[fi]);
        float r = avm_sample_to_float(b, fmts[fi]);
        if (fmts[fi] == AVM_FMT_FLOAT32) CHECK(r == 1.5f, "Float32 no recorta");
        else CHECK(r <= 1.0f && r > 0.99f, "%s recorta a escala completa (%f)", names[fi], r);

        avm_float_to_sample(-1.0f, b, fmts[fi]);
        r = avm_sample_to_float(b, fmts[fi]);
        CHECK(r == -1.0f, "%s representa -1.0 exactamente", names[fi]);

        avm_float_to_sample(NAN, b, fmts[fi]);
        r = avm_sample_to_float(b, fmts[fi]);
        CHECK(r == 0.0f, "%s convierte NaN en 0", names[fi]);
    }
    CHECK(avm_format_bytes_per_sample(AVM_FMT_SINT24) == 3, "SInt24 empaquetado en 3 bytes");

    // Envío en SInt16, recepción en SInt24: el valor se conserva.
    avm_engine* e = new_engine();
    int16_t in16[128 * 8];
    for (int i = 0; i < 128 * 8; i++) in16[i] = (int16_t)(1000 + (i % 8));
    avm_engine_write(e, AVM_SRC_GROUP_SMAART, 0, 128, in16, AVM_FMT_SINT16, 8);
    uint8_t out24[64 * 8 * 3];
    avm_engine_read(e, AVM_DST_GROUP_LINK, 32, 64, out24, AVM_FMT_SINT24, 8);
    int ok = 1;
    for (int f = 0; f < 64; f++)
        for (int c = 0; c < 8; c++) {
            const uint8_t* p = out24 + (f * 8 + c) * 3;
            int32_t v = (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16));
            if (v & 0x800000) v -= 0x1000000;
            if (v != (1000 + c) * 256) ok = 0;
        }
    CHECK(ok, "SInt16 -> SInt24 debe conservar el valor exacto (x256)");
    free(e);
}

static void test_music_stereo(void)
{
    avm_engine* e = new_engine();
    avm_matrix m;
    avm_matrix_clear(&m);
    m.cell[AVM_DST_SMAART_BASE + 7][AVM_SRC_MUSIC_BASE + 0] = 1;  // Música L -> Smaart entrada 8
    m.cell[AVM_DST_LINK_BASE + 0][AVM_SRC_MUSIC_BASE + 1] = 1;    // Música R -> envío LISTENTO 1
    avm_engine_set_matrix(e, &m);
    float mus[256 * 2];
    for (int f = 0; f < 256; f++) { mus[f * 2] = 0.3f; mus[f * 2 + 1] = -0.7f; }
    avm_engine_write(e, AVM_SRC_GROUP_MUSIC, 0, 256, mus, AVM_FMT_FLOAT32, 2);
    float s[32 * 8], l[32 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 10, 32, s, AVM_FMT_FLOAT32, 8);
    avm_engine_read(e, AVM_DST_GROUP_LINK, 10, 32, l, AVM_FMT_FLOAT32, 8);
    int ok = 1;
    for (int f = 0; f < 32; f++)
        for (int c = 0; c < 8; c++) {
            if (s[f * 8 + c] != (c == 7 ? 0.3f : 0.0f)) ok = 0;
            if (l[f * 8 + c] != (c == 0 ? -0.7f : 0.0f)) ok = 0;
        }
    CHECK(ok, "la música estéreo debe llegar solo a los destinos elegidos, L y R por separado");
    free(e);
}

// --- Concurrencia: un hilo escribe, otro lee detrás, como en el driver real.
typedef struct { avm_engine* e; _Atomic int stop; _Atomic long errors; _Atomic long reads; _Atomic long overruns; } stress_ctx;

static float ramp(int64_t t, int c) { return (float)((t * 7 + c * 131) % 10007) / 10007.0f; }

static void* stress_writer(void* p)
{
    stress_ctx* x = (stress_ctx*)p;
    float buf[256 * 8];
    for (int64_t t = 0; t < 4000000 && !atomic_load(&x->stop); t += 256) {
        for (int f = 0; f < 256; f++)
            for (int c = 0; c < 8; c++) buf[f * 8 + c] = ramp(t + f, c);
        avm_engine_write(x->e, AVM_SRC_GROUP_SMAART, t, 256, buf, AVM_FMT_FLOAT32, 8);
    }
    atomic_store(&x->stop, 1);
    return NULL;
}

static void* stress_reader(void* p)
{
    stress_ctx* x = (stress_ctx*)p;
    float buf[256 * 8];
    while (!atomic_load(&x->stop)) {
        int64_t we = atomic_load(&x->e->written_end[AVM_SRC_GROUP_SMAART]);
        if (we < 8192) continue;
        int64_t t0 = we - 4096;
        if (!avm_engine_read(x->e, AVM_DST_GROUP_LINK, t0, 256, buf, AVM_FMT_FLOAT32, 8)) {
            // El escritor (sin ritmo de tiempo real) dio una vuelta al buffer:
            // el bloque se descartó como silencio, que es lo correcto.
            int silent = 1;
            for (int i = 0; i < 256 * 8; i++) if (buf[i] != 0.0f) silent = 0;
            if (!silent) atomic_fetch_add(&x->errors, 1);
            atomic_fetch_add(&x->overruns, 1);
            continue;
        }
        for (int f = 0; f < 256; f++)
            for (int c = 0; c < 8; c++)
                if (buf[f * 8 + c] != ramp(t0 + f, c)) atomic_fetch_add(&x->errors, 1);
        atomic_fetch_add(&x->reads, 1);
    }
    return NULL;
}

static void test_concurrency(void)
{
    stress_ctx x;
    x.e = new_engine();
    atomic_init(&x.stop, 0);
    atomic_init(&x.errors, 0);
    atomic_init(&x.reads, 0);
    atomic_init(&x.overruns, 0);
    pthread_t w, r;
    pthread_create(&r, NULL, stress_reader, &x);
    pthread_create(&w, NULL, stress_writer, &x);
    pthread_join(w, NULL);
    pthread_join(r, NULL);
    CHECK(atomic_load(&x.errors) == 0, "lectura concurrente sin muestras corruptas (%ld errores en %ld lecturas)",
          atomic_load(&x.errors), atomic_load(&x.reads));
    printf("  concurrencia: %ld lecturas comprobadas, %ld bloques descartados por vuelta del escritor\n",
           atomic_load(&x.reads), atomic_load(&x.overruns));
    free(x.e);
}

// Lector retrasado una vuelta completa: el bloque debe descartarse, no mezclarse.
static void test_overrun_detected(void)
{
    avm_engine* e = new_engine();
    write_constant_sources(e, 0, 2048);
    // Simula un escritor que, durante la lectura, ya escribe una vuelta por delante.
    atomic_store(&e->writing_end[AVM_SRC_GROUP_LINK], (int64_t)AVM_RING_FRAMES + 2048);
    float buf[64 * 8];
    bool ok = avm_engine_read(e, AVM_DST_GROUP_SMAART, 100, 64, buf, AVM_FMT_FLOAT32, 8);
    int silent = 1;
    for (int i = 0; i < 64 * 8; i++) if (buf[i] != 0.0f) silent = 0;
    CHECK(!ok && silent, "un bloque posiblemente sobrescrito se entrega como silencio");
    CHECK(atomic_load(&e->overrun_reads) == 1, "y queda contado");
    // Un grupo no usado por la matriz no invalida la lectura.
    ok = avm_engine_read(e, AVM_DST_GROUP_LINK, 100, 64, buf, AVM_FMT_FLOAT32, 8);
    CHECK(ok && buf[0] == src_value(AVM_SRC_SMAART_BASE), "las fuentes de otros grupos no se ven afectadas");
    free(e);
}

// --- Publicación de la matriz (corrección Codex #2) ------------------------

static bool matrix_equal(const avm_matrix* a, const avm_matrix* b)
{
    return memcmp(a->cell, b->cell, sizeof(a->cell)) == 0;
}

static void make_matrix_a(avm_matrix* m) { avm_matrix_default(m); }
static void make_matrix_b(avm_matrix* m)
{
    avm_matrix_clear(m);
    for (int i = 0; i < 8; i++) m->cell[AVM_DST_SMAART_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;
    m->cell[AVM_DST_LINK_BASE][AVM_SRC_MUSIC_BASE] = 1;
}

static void test_matrix_writer_suspended_on_spare_slot(void)
{
    avm_engine* e = new_engine();
    avm_matrix a, b, got;
    make_matrix_a(&a);
    make_matrix_b(&b);
    avm_engine_set_matrix(e, &a);

    // Simula un escritor detenido a mitad de escribir la ranura siguiente:
    // contador impar y datos a medio copiar.
    uint32_t cur = atomic_load(&e->matrix_published);
    avm_matrix_slot* spare = &e->matrix_slots[(cur + 1) % AVM_MATRIX_SLOTS];
    atomic_fetch_add(&spare->seq, 1);
    atomic_store(&spare->words[0], 0xFFFFFFFFFFFFFFFFull);

    int ok = 1;
    for (int i = 0; i < 100000; i++)
        if (!avm_engine_try_get_matrix(e, &got) || !matrix_equal(&got, &a)) { ok = 0; break; }
    CHECK(ok, "un escritor detenido en una ranura libre no afecta a los lectores");

    // El audio sigue saliendo según la matriz publicada.
    write_constant_sources(e, 0, 2048);
    float buf[64 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 100, 64, buf, AVM_FMT_FLOAT32, 8);
    CHECK(buf[0] == src_value(AVM_SRC_LINK_BASE), "el audio sigue la matriz publicada");
    CHECK(atomic_load(&e->stale_matrix_reads) == 0, "sin lecturas con copia anterior");
    (void)b;
    free(e);
}

static void test_matrix_published_slot_torn_is_bounded(void)
{
    avm_engine* e = new_engine();
    avm_matrix a, b, got;
    make_matrix_a(&a);
    make_matrix_b(&b);
    avm_engine_set_matrix(e, &b);
    write_constant_sources(e, 0, 2048);

    // Primera lectura: la caché del lector toma la matriz B.
    float buf[64 * 8];
    avm_engine_read(e, AVM_DST_GROUP_SMAART, 100, 64, buf, AVM_FMT_FLOAT32, 8);

    // Caso patológico: la ranura publicada queda con escritura "en curso" para siempre.
    uint32_t cur = atomic_load(&e->matrix_published);
    atomic_fetch_add(&e->matrix_slots[cur].seq, 1);

    memset(&got, 0x5A, sizeof(got));
    bool r = avm_engine_try_get_matrix(e, &got);
    CHECK(!r, "con la ranura publicada inestable, la lectura devuelve false en vez de esperar");
    int untouched = 1;
    for (size_t i = 0; i < sizeof(got.cell); i++) if (((uint8_t*)got.cell)[i] != 0x5A) untouched = 0;
    CHECK(untouched, "una lectura fallida no modifica el destino");

    // El hilo de audio no se bloquea y usa la última copia válida (B).
    for (int i = 0; i < 1000; i++)
        avm_engine_read(e, AVM_DST_GROUP_SMAART, 100, 64, buf, AVM_FMT_FLOAT32, 8);
    CHECK(buf[0] == src_value(AVM_SRC_SMAART_BASE), "el audio usa la última matriz coherente");
    CHECK(atomic_load(&e->stale_matrix_reads) == 1000, "cada lectura sin copia coherente queda contada (%llu)",
          (unsigned long long)atomic_load(&e->stale_matrix_reads));

    // Un escritor nuevo publica otra ranura y todo vuelve a la normalidad.
    avm_engine_set_matrix(e, &a);
    CHECK(avm_engine_try_get_matrix(e, &got) && matrix_equal(&got, &a), "una nueva publicación se lee de inmediato");
    (void)b;
    free(e);
}

typedef struct {
    avm_engine* e;
    _Atomic int stop;
    _Atomic long torn;
    _Atomic long reads;
    _Atomic long misses;
} snap_ctx;

static void* snap_writer(void* p)
{
    snap_ctx* x = (snap_ctx*)p;
    avm_matrix a, b;
    make_matrix_a(&a);
    make_matrix_b(&b);
    for (int i = 0; i < 300000; i++)
        avm_engine_set_matrix(x->e, (i & 1) ? &b : &a);
    atomic_store(&x->stop, 1);
    return NULL;
}

static void* snap_reader(void* p)
{
    snap_ctx* x = (snap_ctx*)p;
    avm_matrix a, b, got;
    make_matrix_a(&a);
    make_matrix_b(&b);
    while (!atomic_load(&x->stop)) {
        if (avm_engine_try_get_matrix(x->e, &got)) {
            if (!matrix_equal(&got, &a) && !matrix_equal(&got, &b)) atomic_fetch_add(&x->torn, 1);
            atomic_fetch_add(&x->reads, 1);
        } else {
            atomic_fetch_add(&x->misses, 1);
        }
    }
    return NULL;
}

static void test_matrix_snapshot_concurrency(void)
{
    snap_ctx x;
    x.e = new_engine();
    atomic_init(&x.stop, 0);
    atomic_init(&x.torn, 0);
    atomic_init(&x.reads, 0);
    atomic_init(&x.misses, 0);
    pthread_t w, r1, r2;
    pthread_create(&r1, NULL, snap_reader, &x);
    pthread_create(&r2, NULL, snap_reader, &x);
    pthread_create(&w, NULL, snap_writer, &x);
    pthread_join(w, NULL);
    pthread_join(r1, NULL);
    pthread_join(r2, NULL);
    CHECK(atomic_load(&x.torn) == 0, "ninguna copia mezcla dos matrices (%ld mezcladas)", atomic_load(&x.torn));
    CHECK(atomic_load(&x.reads) > 0, "los lectores obtuvieron copias");
    printf("  matriz concurrente: %ld copias coherentes, %ld lecturas acotadas sin copia\n",
           atomic_load(&x.reads), atomic_load(&x.misses));
    free(x.e);
}

int main(void)
{
    test_rules();
    test_isolation_default();
    test_measurement_preset_cells();
    test_clear_is_silence();
    test_single_route_and_sum();
    test_forbidden_ignored_even_if_forced();
    test_stale_data();
    test_formats();
    test_music_stereo();
    test_concurrency();
    test_overrun_detected();
    test_matrix_writer_suspended_on_spare_slot();
    test_matrix_published_slot_torn_is_bounded();
    test_matrix_snapshot_concurrency();
    printf("Resultado: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
