// Pruebas de la transacción de cambio de frecuencia (corrección Codex #1).

#include "../Driver/src/avm_rate.h"

#include <stdio.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } else { g_fail++; printf("FALLO %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define NDEV 3
#define TIMEOUT 1000

static void fresh(avm_rate_state* st) { avm_rate_init(st, NDEV, 48000.0, TIMEOUT); }

static void test_basic_and_validation(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    CHECK(avm_rate_begin(&st, 22050.0, 0, &gen) == AVM_RATE_UNSUPPORTED, "frecuencia no admitida");
    CHECK(avm_rate_begin(&st, 48000.0, 0, &gen) == AVM_RATE_SAME, "misma frecuencia: nada que hacer");
    CHECK(!avm_rate_pending(&st), "sin transacción tras SAME");

    CHECK(avm_rate_begin(&st, 96000.0, 0, &gen) == AVM_RATE_OK, "inicio correcto");
    CHECK(avm_rate_begin(&st, 44100.0, 0, &gen) == AVM_RATE_BUSY, "segunda transacción simultánea rechazada");
    for (uint32_t d = 0; d < NDEV; d++) {
        CHECK(st.current_rate == (d == 0 ? 48000.0 : 96000.0), "la frecuencia cambia al primer Perform");
        CHECK(avm_rate_perform(&st, d, gen, 96000.0) == AVM_RATE_OK, "perform %u", d);
    }
    CHECK(st.current_rate == 96000.0 && !avm_rate_pending(&st) && !st.inconsistent, "transacción completa y coherente");
    CHECK(avm_rate_perform(&st, 0, gen, 96000.0) == AVM_RATE_STALE, "perform repetido tras cerrar: obsoleto");
}

static void test_rejected_with_io(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    CHECK(avm_rate_io_start(&st, 1, 0) == AVM_RATE_OK, "IO en LISTENTO");
    CHECK(avm_rate_begin(&st, 96000.0, 0, &gen) == AVM_RATE_IO_ACTIVE, "con IO en cualquier dispositivo se rechaza");
    CHECK(st.current_rate == 48000.0 && !avm_rate_pending(&st), "nada cambia");
    avm_rate_io_stop(&st, 1);
    CHECK(avm_rate_begin(&st, 96000.0, 0, &gen) == AVM_RATE_OK, "sin IO se admite");
}

static void test_io_blocked_during_transaction(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 44100.0, 0, &gen);
    avm_rate_perform(&st, 0, gen, 44100.0);
    CHECK(avm_rate_io_start(&st, 2, 1) == AVM_RATE_BUSY, "StartIO rechazado durante la transacción");
    CHECK(st.running_total == 0, "el arranque rechazado no se cuenta");
    avm_rate_perform(&st, 1, gen, 44100.0);
    avm_rate_perform(&st, 2, gen, 44100.0);
    CHECK(avm_rate_io_start(&st, 2, 2) == AVM_RATE_OK, "StartIO admitido al terminar");
}

static void test_perform_with_io_is_refused(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    st.running_total = 1;  // simula IO que arrancó por un camino no previsto
    CHECK(avm_rate_perform(&st, 0, gen, 96000.0) == AVM_RATE_IO_ACTIVE, "Perform con IO activa no cambia el reloj");
    CHECK(st.current_rate == 48000.0, "la frecuencia sigue igual");
}

static void test_abort_before_any_perform(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    avm_rate_abort_result r = avm_rate_abort(&st, 1, gen, 0);
    CHECK(!r.revert_needed, "sin nada aplicado no hace falta revertir");
    CHECK(!avm_rate_pending(&st) && st.current_rate == 48000.0 && !st.inconsistent, "estado intacto");
    CHECK(avm_rate_perform(&st, 0, gen, 96000.0) == AVM_RATE_STALE, "un Perform tardío de la transacción cancelada se rechaza");
    CHECK(st.current_rate == 48000.0, "y no cambia nada");
}

static void test_abort_after_partial_perform_reverts(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);
    CHECK(st.current_rate == 96000.0, "un dispositivo ya aplicó 96 kHz");
    avm_rate_abort_result r = avm_rate_abort(&st, 1, gen, 10);
    CHECK(r.revert_needed && r.revert_rate == 48000.0 && r.revert_generation != gen, "se abre una reversión a 48 kHz");
    CHECK(avm_rate_pending(&st) && st.is_revert, "la reversión es la transacción vigente");
    CHECK(avm_rate_io_start(&st, 0, 11) == AVM_RATE_BUSY, "StartIO sigue bloqueado durante la reversión");
    CHECK(avm_rate_perform(&st, 2, gen, 96000.0) == AVM_RATE_STALE, "un Perform de la transacción cancelada se rechaza");
    CHECK(avm_rate_abort(&st, 2, gen, 12).revert_needed == false, "un Abort de la transacción vieja se ignora");
    CHECK(avm_rate_pending(&st), "y no cierra la reversión");
    for (uint32_t d = 0; d < NDEV; d++)
        CHECK(avm_rate_perform(&st, d, r.revert_generation, 48000.0) == AVM_RATE_OK, "reversión perform %u", d);
    CHECK(st.current_rate == 48000.0 && !avm_rate_pending(&st) && !st.inconsistent, "reversión completa y coherente");
}

// Deja el estado inconsistente: cambio aplicado en un dispositivo y reversión cancelada.
static void make_inconsistent_by_failed_revert(avm_rate_state* st)
{
    uint32_t gen = 0;
    fresh(st);
    avm_rate_begin(st, 96000.0, 0, &gen);
    avm_rate_perform(st, 0, gen, 96000.0);
    avm_rate_abort_result r = avm_rate_abort(st, 1, gen, 0);
    avm_rate_abort(st, 0, r.revert_generation, 0);
}

// Recupera pidiendo la frecuencia actual y aplicándola en los tres dispositivos.
static void recover_same_rate(avm_rate_state* st, uint64_t now)
{
    uint32_t g = 0;
    double rate = st->current_rate;
    CHECK(avm_rate_begin(st, rate, now, &g) == AVM_RATE_OK,
          "en estado inconsistente, pedir la frecuencia actual abre una recuperación (no SAME)");
    CHECK(avm_rate_io_start(st, 0, now) == AVM_RATE_BUSY, "durante la recuperación el audio sigue bloqueado");
    for (uint32_t d = 0; d < NDEV; d++) {
        CHECK(st->inconsistent, "sigue inconsistente hasta que aplican los tres (faltan %u)", NDEV - d);
        CHECK(avm_rate_perform(st, d, g, rate) == AVM_RATE_OK, "recuperación perform %u", d);
    }
    CHECK(!st->inconsistent && !avm_rate_pending(st), "recuperación completa: coherente");
}

static void test_revert_aborted_blocks_until_recovery(void)
{
    avm_rate_state st;
    make_inconsistent_by_failed_revert(&st);
    CHECK(st.inconsistent && !avm_rate_pending(&st), "reversión cancelada: inconsistente y sin transacción");
    CHECK(avm_rate_health(&st, 1) == 2, "el estado informa inconsistente");
    for (uint32_t d = 0; d < NDEV; d++)
        CHECK(avm_rate_io_start(&st, d, 1) == AVM_RATE_INCONSISTENT, "StartIO bloqueado en el dispositivo %u", d);
    CHECK(st.running_total == 0, "ningún arranque contado");
    CHECK(avm_rate_io_start(&st, 0, 1 + 10 * TIMEOUT) == AVM_RATE_INCONSISTENT, "el paso del tiempo no desbloquea");

    recover_same_rate(&st, 2);
    CHECK(avm_rate_health(&st, 3) == 0, "el estado informa coherente");
    CHECK(avm_rate_io_start(&st, 0, 3) == AVM_RATE_OK, "tras la recuperación completa el audio arranca");
}

static void test_recovery_to_other_rate(void)
{
    avm_rate_state st;
    uint32_t g = 0;
    make_inconsistent_by_failed_revert(&st);
    CHECK(avm_rate_begin(&st, 44100.0, 1, &g) == AVM_RATE_OK, "también se recupera pidiendo otra frecuencia");
    for (uint32_t d = 0; d < NDEV; d++) avm_rate_perform(&st, d, g, 44100.0);
    CHECK(!st.inconsistent && st.current_rate == 44100.0, "los tres a 44,1 kHz y coherente");
    CHECK(avm_rate_io_start(&st, 1, 2) == AVM_RATE_OK, "el audio arranca");
}

static void test_recovery_incomplete_keeps_blocking(void)
{
    avm_rate_state st;
    uint32_t g = 0;
    make_inconsistent_by_failed_revert(&st);

    // Recuperación cancelada sin ningún Perform: sigue inconsistente.
    avm_rate_begin(&st, st.current_rate, 1, &g);
    avm_rate_abort(&st, 0, g, 1);
    CHECK(st.inconsistent && avm_rate_io_start(&st, 0, 2) == AVM_RATE_INCONSISTENT,
          "recuperación cancelada antes de aplicar: sigue bloqueado");

    // Recuperación caducada sin ningún Perform: sigue inconsistente.
    avm_rate_begin(&st, st.current_rate, 10, &g);
    CHECK(avm_rate_io_start(&st, 0, 11 + TIMEOUT) == AVM_RATE_INCONSISTENT,
          "recuperación caducada sin aplicar: sigue bloqueado");

    // Recuperación aplicada solo en parte y caducada: sigue inconsistente.
    avm_rate_begin(&st, st.current_rate, 5000, &g);
    avm_rate_perform(&st, 0, g, st.current_rate);
    CHECK(avm_rate_io_start(&st, 0, 5001 + TIMEOUT) == AVM_RATE_INCONSISTENT,
          "recuperación parcial caducada: sigue bloqueado");

    recover_same_rate(&st, 9000);
    CHECK(avm_rate_io_start(&st, 0, 9001) == AVM_RATE_OK, "una recuperación completa desbloquea");
}

static void test_same_rate_when_consistent(void)
{
    avm_rate_state st;
    uint32_t g = 0;
    fresh(&st);
    CHECK(avm_rate_begin(&st, 48000.0, 0, &g) == AVM_RATE_SAME, "coherente y misma frecuencia: SAME");
    CHECK(!avm_rate_pending(&st), "sin transacción");
}

static void test_expiry(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);

    // Cambio caducado sin ningún Perform: nada cambió, el audio puede arrancar.
    avm_rate_begin(&st, 96000.0, 100, &gen);
    CHECK(avm_rate_io_start(&st, 0, 100 + TIMEOUT) == AVM_RATE_BUSY, "dentro del plazo sigue bloqueado");
    CHECK(avm_rate_io_start(&st, 0, 101 + TIMEOUT) == AVM_RATE_OK, "un cambio sin aplicar caduca y libera");
    CHECK(!st.inconsistent && st.current_rate == 48000.0, "sin nada aplicado, la frecuencia sigue siendo la anterior");
    avm_rate_io_stop(&st, 0);

    // Cambio caducado tras aplicarse en un solo dispositivo: discrepancia, se bloquea.
    avm_rate_begin(&st, 96000.0, 5000, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);
    CHECK(avm_rate_io_start(&st, 1, 5001 + TIMEOUT) == AVM_RATE_INCONSISTENT,
          "cambio parcial caducado: StartIO bloqueado");
    CHECK(st.inconsistent, "y queda marcado inconsistente");
    recover_same_rate(&st, 6000 + TIMEOUT);
    CHECK(avm_rate_io_start(&st, 1, 6001 + TIMEOUT) == AVM_RATE_OK, "tras recuperar, arranca");
}

static void test_revert_expired_without_perform(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);          // un dispositivo a 96 kHz
    avm_rate_abort_result r = avm_rate_abort(&st, 1, gen, 10);  // se abre la reversión
    CHECK(r.revert_needed && st.is_revert, "reversión abierta");
    // El host nunca llama a Perform de la reversión: caduca.
    CHECK(avm_rate_health(&st, 11 + TIMEOUT) == 2, "reversión caducada sin Perform: inconsistente");
    CHECK(st.inconsistent && !avm_rate_pending(&st), "marcado inconsistente y sin transacción");
    CHECK(avm_rate_io_start(&st, 0, 12 + TIMEOUT) == AVM_RATE_INCONSISTENT, "StartIO bloqueado");
    recover_same_rate(&st, 20 + TIMEOUT);
    CHECK(avm_rate_io_start(&st, 0, 21 + TIMEOUT) == AVM_RATE_OK, "tras recuperar, arranca");
}

static void test_revert_request_failure(void)
{
    // El driver, si falla la petición de reversión a un dispositivo, llama a
    // avm_rate_abort con la generación de la reversión (AVM_IssueRateRevert).
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);
    avm_rate_abort_result r = avm_rate_abort(&st, 2, gen, 0);
    avm_rate_perform(&st, 0, r.revert_generation, 48000.0);  // un dispositivo ya revirtió
    avm_rate_abort(&st, 1, r.revert_generation, 1);          // la petición al siguiente falla
    CHECK(st.inconsistent, "fallo de la reversión: inconsistente");
    CHECK(avm_rate_io_start(&st, 2, 2) == AVM_RATE_INCONSISTENT, "StartIO bloqueado");
    recover_same_rate(&st, 3);
    CHECK(avm_rate_io_start(&st, 2, 4) == AVM_RATE_OK, "tras recuperar, arranca");
}

static void test_generation_never_zero(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    st.generation = 0x00FFFFFFu;
    avm_rate_begin(&st, 96000.0, 0, &gen);
    CHECK(gen == 1, "la generación da la vuelta sin usar 0 (%u)", gen);
}

int main(void)
{
    test_basic_and_validation();
    test_rejected_with_io();
    test_io_blocked_during_transaction();
    test_perform_with_io_is_refused();
    test_abort_before_any_perform();
    test_abort_after_partial_perform_reverts();
    test_revert_aborted_blocks_until_recovery();
    test_recovery_to_other_rate();
    test_recovery_incomplete_keeps_blocking();
    test_same_rate_when_consistent();
    test_expiry();
    test_revert_expired_without_perform();
    test_revert_request_failure();
    test_generation_never_zero();
    printf("Resultado: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
