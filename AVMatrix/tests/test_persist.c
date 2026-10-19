// Pruebas del guardado ordenado (Driver/src/avm_persist.c) con el mixer y la
// matriz reales del motor. El almacenamiento es simulado y deliberadamente
// desordenado: cada escritura tarda un tiempo distinto, para que un guardado
// lento de un cambio antiguo termine DESPUÉS del de un cambio nuevo.
// Comprueba que lo guardado es siempre el último ajuste aplicado.

#define _POSIX_C_SOURCE 200809L
#include "../Driver/src/avm_core.h"
#include "../Driver/src/avm_persist.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int mixer_eq(const avm_mixer* a, const avm_mixer* b)
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++)
        if (a->src_gain[i] != b->src_gain[i] || a->src_mute[i] != b->src_mute[i]) return 0;
    for (int i = 0; i < AVM_NUM_DESTS; i++)
        if (a->dst_gain[i] != b->dst_gain[i] || a->dst_mute[i] != b->dst_mute[i]) return 0;
    return 1;
}

// --- Almacenamiento simulado ---------------------------------------------------
// La escritura copia el valor, espera `delay_ms` y entonces lo «confirma» como
// guardado. Así un guardado lento iniciado antes termina después.
typedef struct {
    pthread_mutex_t lock;
    avm_mixer       saved_mixer;
    avm_matrix      saved_matrix;
    int             writes;
    _Atomic int     delay_first_ms;   // retraso de la próxima escritura (una vez)
    _Atomic int     random_delays;    // 1 = retrasos aleatorios en todas
} fake_storage;

static fake_storage g_st;

static int next_delay(void)
{
    int d = atomic_exchange(&g_st.delay_first_ms, 0);
    if (d) return d;
    if (atomic_load(&g_st.random_delays)) return rand() % 3;
    return 0;
}

static void store_mixer(const void* snap, void* ctx)
{
    (void)ctx;
    avm_mixer v = *(const avm_mixer*)snap;
    sleep_ms(next_delay());
    pthread_mutex_lock(&g_st.lock);
    g_st.saved_mixer = v;
    g_st.writes++;
    pthread_mutex_unlock(&g_st.lock);
}

static void store_matrix(const void* snap, void* ctx)
{
    (void)ctx;
    avm_matrix v = *(const avm_matrix*)snap;
    sleep_ms(next_delay());
    pthread_mutex_lock(&g_st.lock);
    g_st.saved_matrix = v;
    g_st.writes++;
    pthread_mutex_unlock(&g_st.lock);
}

static bool apply_mixer(void* obj, const void* v) { return avm_engine_set_mixer((avm_engine*)obj, (const avm_mixer*)v); }
static void snap_mixer(void* obj, void* out) { avm_engine_get_mixer((const avm_engine*)obj, (avm_mixer*)out); }
static bool apply_matrix(void* obj, const void* v) { avm_engine_set_matrix((avm_engine*)obj, (const avm_matrix*)v); return true; }
static void snap_matrix(void* obj, void* out) { avm_engine_try_get_matrix((avm_engine*)obj, (avm_matrix*)out); }

static pthread_mutex_t g_state = PTHREAD_MUTEX_INITIALIZER;
static avm_persist g_pm, g_px;
static avm_engine* g_e;

static bool commit_mixer(const avm_mixer* mx)
{
    avm_mixer snap;
    return avm_persist_commit(&g_pm, apply_mixer, snap_mixer, store_mixer, g_e, mx, &snap, NULL);
}

// Lo que hacía ee604ad: aplicar bajo el mutex y guardar el valor pedido fuera.
static bool commit_mixer_naive(const avm_mixer* mx)
{
    pthread_mutex_lock(&g_state);
    bool ok = avm_engine_set_mixer(g_e, mx);
    pthread_mutex_unlock(&g_state);
    if (ok) store_mixer(mx, NULL);
    return ok;
}

typedef struct { avm_mixer mx; int start_delay_ms; bool naive; } job;

static void* run_job(void* p)
{
    job* j = (job*)p;
    sleep_ms(j->start_delay_ms);
    if (j->naive) commit_mixer_naive(&j->mx); else commit_mixer(&j->mx);
    return NULL;
}

// Escenario de Codex: A (MUTE = 0) se aplica primero pero su guardado tarda;
// B (MUTE = 1) se aplica después y se guarda rápido.
static void scenario(bool naive, avm_mixer* saved, avm_mixer* current)
{
    avm_mixer mx; avm_mixer_default(&mx);
    commit_mixer(&mx);
    job a, b;
    avm_mixer_default(&a.mx); a.mx.dst_mute[AVM_DST_SMAART_BASE + 7] = 0; a.start_delay_ms = 0; a.naive = naive;
    avm_mixer_default(&b.mx); b.mx.dst_mute[AVM_DST_SMAART_BASE + 7] = 1; b.start_delay_ms = 20; b.naive = naive;
    atomic_store(&g_st.delay_first_ms, 120);   // el guardado de A (el primero) es lento
    pthread_t ta, tb;
    pthread_create(&ta, NULL, run_job, &a);
    pthread_create(&tb, NULL, run_job, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    pthread_mutex_lock(&g_state);
    avm_engine_get_mixer(g_e, current);
    pthread_mutex_unlock(&g_state);
    pthread_mutex_lock(&g_st.lock);
    *saved = g_st.saved_mixer;
    pthread_mutex_unlock(&g_st.lock);
}

static void test_codex_scenario(void)
{
    avm_mixer saved, cur;
    // Con el método anterior el fallo se reproduce (la prueba es sensible a él).
    scenario(true, &saved, &cur);
    CHECK(cur.dst_mute[7] == 1 && saved.dst_mute[7] == 0,
          "método anterior: se reproduce el fallo de Codex (MUTE vigente %d, guardado %d)", cur.dst_mute[7], saved.dst_mute[7]);
    // Con el guardado ordenado, lo guardado es lo vigente.
    scenario(false, &saved, &cur);
    CHECK(cur.dst_mute[7] == 1 && saved.dst_mute[7] == 1 && mixer_eq(&saved, &cur),
          "guardado ordenado: MUTE vigente %d, guardado %d (iguales)", cur.dst_mute[7], saved.dst_mute[7]);
}

// Otro orden posible: A se aplica, su hilo se retrasa ANTES de guardar, B se
// aplica y se guarda, y después A guarda. Guardar el valor pedido dejaría A;
// el guardado ordenado guarda el estado vigente (B) o no vuelve a escribir.
static _Atomic int g_slow_after_apply;
static void hook_after_apply(void* ctx)
{
    (void)ctx;
    if (atomic_exchange(&g_slow_after_apply, 0)) sleep_ms(120);
}

static void test_delay_between_apply_and_store(void)
{
    avm_mixer mx; avm_mixer_default(&mx);
    commit_mixer(&mx);
    g_pm.after_apply = hook_after_apply;
    atomic_store(&g_slow_after_apply, 1);           // el primero (A) se retrasa
    job a, b;
    avm_mixer_default(&a.mx); a.mx.src_mute[AVM_SRC_MUSIC_BASE] = 0; a.mx.src_gain[AVM_SRC_MUSIC_BASE] = 0.5f; a.start_delay_ms = 0; a.naive = false;
    avm_mixer_default(&b.mx); b.mx.src_mute[AVM_SRC_MUSIC_BASE] = 1; b.start_delay_ms = 30; b.naive = false;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, run_job, &a);
    pthread_create(&tb, NULL, run_job, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    g_pm.after_apply = NULL;
    avm_mixer cur; avm_engine_get_mixer(g_e, &cur);
    CHECK(cur.src_mute[AVM_SRC_MUSIC_BASE] == 1, "vigente: el último aplicado (B, MUTE)");
    CHECK(mixer_eq(&g_st.saved_mixer, &cur), "guardado = vigente aunque A guarde después de B (MUTE guardado %d)", g_st.saved_mixer.src_mute[AVM_SRC_MUSIC_BASE]);
}

// Tercer orden: B se aplica mientras A está entre aplicar y guardar, y A toma
// el turno de guardado ANTES que B. A debe guardar el estado vigente (B), no su
// propio valor; si no, B (que ya no es más nuevo) no volvería a escribir.
static _Atomic int g_b_applied;
static void hook_interleave(void* ctx)
{
    if (ctx == (void*)1) {          // A: espera a que B se haya aplicado
        for (int i = 0; i < 1000 && !atomic_load(&g_b_applied); i++) sleep_ms(1);
    } else if (ctx == (void*)2) {   // B: avisa y deja que A guarde primero
        atomic_store(&g_b_applied, 1);
        sleep_ms(80);
    }
}

typedef struct { avm_mixer mx; void* ctx; int start_delay_ms; } job2;
static void* run_job2(void* p)
{
    job2* j = (job2*)p;
    sleep_ms(j->start_delay_ms);
    avm_mixer snap;
    avm_persist_commit(&g_pm, apply_mixer, snap_mixer, store_mixer, g_e, &j->mx, &snap, j->ctx);
    return NULL;
}

static void test_store_order_interleaving(void)
{
    avm_mixer mx; avm_mixer_default(&mx);
    commit_mixer(&mx);
    atomic_store(&g_b_applied, 0);
    g_pm.after_apply = hook_interleave;
    job2 a, b;
    avm_mixer_default(&a.mx); a.mx.dst_gain[AVM_DST_LINK_BASE] = 0.25f; a.ctx = (void*)1; a.start_delay_ms = 0;
    avm_mixer_default(&b.mx); b.mx.dst_mute[AVM_DST_LINK_BASE] = 1;     b.ctx = (void*)2; b.start_delay_ms = 20;
    pthread_t ta, tb;
    pthread_create(&ta, NULL, run_job2, &a);
    pthread_create(&tb, NULL, run_job2, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    g_pm.after_apply = NULL;
    avm_mixer cur; avm_engine_get_mixer(g_e, &cur);
    CHECK(cur.dst_mute[AVM_DST_LINK_BASE] == 1 && cur.dst_gain[AVM_DST_LINK_BASE] == 1.0f, "vigente: B");
    CHECK(mixer_eq(&g_st.saved_mixer, &cur), "A guarda primero, pero guarda el estado vigente (B): guardado = vigente");
}

// Muchos hilos cambiando a la vez, con guardados de duración aleatoria.
typedef struct { int id; int n; } stress_arg;

static void* stress(void* p)
{
    stress_arg* a = (stress_arg*)p;
    unsigned seed = (unsigned)(a->id * 7919 + 1);
    for (int i = 0; i < a->n; i++) {
        avm_mixer mx; avm_mixer_default(&mx);
        for (int c = 0; c < AVM_NUM_SOURCES; c++) { mx.src_gain[c] = (float)(rand_r(&seed) % 101) / 100.0f; mx.src_mute[c] = (uint8_t)(rand_r(&seed) & 1); }
        for (int c = 0; c < AVM_NUM_DESTS; c++)   { mx.dst_gain[c] = (float)(rand_r(&seed) % 101) / 100.0f; mx.dst_mute[c] = (uint8_t)(rand_r(&seed) & 1); }
        commit_mixer(&mx);
        if (i % 4 == 0) {
            avm_matrix m; avm_matrix_clear(&m);
            for (int d = 0; d < AVM_NUM_DESTS; d++)
                for (int s = 0; s < AVM_NUM_SOURCES; s++)
                    if (rand_r(&seed) % 5 == 0 && avm_route_allowed(d, s) == AVM_ROUTE_OK) m.cell[d][s] = 1;
            avm_matrix snap;
            avm_persist_commit(&g_px, apply_matrix, snap_matrix, store_matrix, g_e, &m, &snap, NULL);
        }
    }
    return NULL;
}

static void test_stress(void)
{
    atomic_store(&g_st.random_delays, 1);
    pthread_mutex_lock(&g_st.lock); g_st.writes = 0; pthread_mutex_unlock(&g_st.lock);
    pthread_t th[8];
    stress_arg args[8];
    for (int i = 0; i < 8; i++) { args[i].id = i; args[i].n = 60; pthread_create(&th[i], NULL, stress, &args[i]); }
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    atomic_store(&g_st.random_delays, 0);
    avm_mixer cur; avm_matrix curm;
    pthread_mutex_lock(&g_state);
    avm_engine_get_mixer(g_e, &cur);
    avm_engine_try_get_matrix(g_e, &curm);
    uint64_t ap = g_pm.applied, st = g_pm.stored;
    pthread_mutex_unlock(&g_state);
    CHECK(mixer_eq(&g_st.saved_mixer, &cur), "8 hilos × 60 cambios con guardados desordenados: el mixer guardado es el vigente");
    CHECK(!memcmp(g_st.saved_matrix.cell, curm.cell, sizeof(curm.cell)), "la matriz guardada es la vigente");
    CHECK(ap == st, "la última generación aplicada (%llu) es la guardada (%llu)", (unsigned long long)ap, (unsigned long long)st);
    CHECK(g_st.writes > 0 && g_st.writes <= 8 * 60 + 8 * 15, "guardados sin duplicar de más (%d)", g_st.writes);
}

static void test_rejected(void)
{
    avm_mixer ok; avm_mixer_default(&ok);
    commit_mixer(&ok);
    int w0 = g_st.writes;
    avm_mixer bad; avm_mixer_default(&bad);
    bad.src_gain[0] = 2.0f;   // amplificación: rechazada
    CHECK(!commit_mixer(&bad), "un ajuste no válido se rechaza");
    CHECK(g_st.writes == w0, "un ajuste rechazado no escribe en el almacenamiento");
    avm_mixer cur; avm_engine_get_mixer(g_e, &cur);
    CHECK(mixer_eq(&cur, &ok) && mixer_eq(&g_st.saved_mixer, &ok), "ni el estado vigente ni el guardado cambian");
}

int main(void)
{
    pthread_mutex_init(&g_st.lock, NULL);
    atomic_init(&g_st.delay_first_ms, 0);
    atomic_init(&g_st.random_delays, 0);
    g_e = (avm_engine*)malloc(sizeof(avm_engine));
    avm_engine_init(g_e);
    avm_persist_init(&g_pm, &g_state);
    avm_persist_init(&g_px, &g_state);
    test_codex_scenario();
    test_rejected();
    test_delay_between_apply_and_store();
    test_store_order_interleaving();
    test_stress();
    printf("Guardado: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    free(g_e);
    return g_fail ? 1 : 0;
}
