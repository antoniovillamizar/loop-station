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

static void test_revert_aborted_marks_inconsistent(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 0, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);
    avm_rate_abort_result r = avm_rate_abort(&st, 1, gen, 0);
    avm_rate_abort_result r2 = avm_rate_abort(&st, 0, r.revert_generation, 0);
    CHECK(!r2.revert_needed, "no se encadena otra reversión");
    CHECK(st.inconsistent && !avm_rate_pending(&st), "queda marcado inconsistente y liberado");
    CHECK(avm_rate_io_start(&st, 0, 1) == AVM_RATE_OK, "el driver no queda bloqueado");
    avm_rate_io_stop(&st, 0);
    uint32_t g3 = 0;
    CHECK(avm_rate_begin(&st, 44100.0, 2, &g3) == AVM_RATE_OK, "un cambio nuevo se admite");
    for (uint32_t d = 0; d < NDEV; d++) avm_rate_perform(&st, d, g3, 44100.0);
    CHECK(!st.inconsistent, "un cambio completo en los tres dispositivos vuelve a dejarlo coherente");
}

static void test_expiry(void)
{
    avm_rate_state st;
    uint32_t gen = 0;
    fresh(&st);
    avm_rate_begin(&st, 96000.0, 100, &gen);
    CHECK(avm_rate_io_start(&st, 0, 100 + TIMEOUT) == AVM_RATE_BUSY, "dentro del plazo sigue bloqueado");
    CHECK(avm_rate_io_start(&st, 0, 101 + TIMEOUT) == AVM_RATE_OK, "una transacción abandonada caduca");
    CHECK(!st.inconsistent, "sin nada aplicado, caducar no es inconsistente");
    avm_rate_io_stop(&st, 0);

    avm_rate_begin(&st, 96000.0, 5000, &gen);
    avm_rate_perform(&st, 0, gen, 96000.0);
    uint32_t g2 = 0;
    CHECK(avm_rate_begin(&st, 44100.0, 5000 + TIMEOUT + 1, &g2) == AVM_RATE_OK, "tras caducar se puede iniciar otra");
    CHECK(st.inconsistent, "caducar a medias se marca inconsistente");
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
    test_revert_aborted_marks_inconsistent();
    test_expiry();
    test_generation_never_zero();
    printf("Resultado: %d comprobaciones correctas, %d fallos\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
