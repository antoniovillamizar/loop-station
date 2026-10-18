// AV Matrix — transacción de cambio de frecuencia. Ver avm_rate.h.

#include "avm_rate.h"

#include <string.h>

#define AVM_GEN_MASK 0x00FFFFFFu

static const double kRates[] = { 44100.0, 48000.0, 96000.0 };

bool avm_rate_is_supported(double rate)
{
    for (unsigned i = 0; i < sizeof(kRates) / sizeof(kRates[0]); i++)
        if (kRates[i] == rate) return true;
    return false;
}

void avm_rate_init(avm_rate_state* st, uint32_t device_count, double rate, uint64_t timeout)
{
    memset(st, 0, sizeof(*st));
    st->device_count = device_count > AVM_RATE_MAX_DEVICES ? AVM_RATE_MAX_DEVICES : device_count;
    st->current_rate = avm_rate_is_supported(rate) ? rate : 48000.0;
    st->timeout = timeout;
}

bool avm_rate_pending(const avm_rate_state* st)
{
    return st->pending_rate != 0.0;
}

static uint32_t all_mask(const avm_rate_state* st)
{
    return st->device_count >= 32 ? 0xFFFFFFFFu : ((1u << st->device_count) - 1u);
}

static void next_generation(avm_rate_state* st)
{
    st->generation = (st->generation + 1) & AVM_GEN_MASK;
    if (st->generation == 0) st->generation = 1;
}

static void close_transaction(avm_rate_state* st)
{
    st->pending_rate = 0.0;
    st->performed_mask = 0;
    st->is_revert = false;
}

// Una transacción abandonada caduca: si quedó aplicada a medias, se marca inconsistente.
static void expire_if_needed(avm_rate_state* st, uint64_t now)
{
    if (!avm_rate_pending(st)) return;
    if (st->timeout == 0 || now < st->started_at || now - st->started_at <= st->timeout) return;
    if (st->performed_mask != 0 && st->performed_mask != all_mask(st))
        st->inconsistent = true;
    close_transaction(st);
}

avm_rate_status avm_rate_begin(avm_rate_state* st, double rate, uint64_t now, uint32_t* out_generation)
{
    if (!avm_rate_is_supported(rate)) return AVM_RATE_UNSUPPORTED;
    expire_if_needed(st, now);
    if (avm_rate_pending(st)) return AVM_RATE_BUSY;
    if (st->running_total > 0) return AVM_RATE_IO_ACTIVE;
    if (rate == st->current_rate) return AVM_RATE_SAME;

    next_generation(st);
    st->previous_rate = st->current_rate;
    st->pending_rate = rate;
    st->performed_mask = 0;
    st->is_revert = false;
    st->started_at = now;
    if (out_generation) *out_generation = st->generation;
    return AVM_RATE_OK;
}

avm_rate_status avm_rate_perform(avm_rate_state* st, uint32_t device, uint32_t generation, double rate)
{
    if (device >= st->device_count) return AVM_RATE_STALE;
    if (!avm_rate_pending(st) || generation != st->generation || rate != st->pending_rate)
        return AVM_RATE_STALE;
    // Otro dispositivo pudo arrancar IO antes de la transacción si el host la
    // aplazó; en ese caso no se cambia el reloj bajo sus pies.
    if (st->running_total > 0) return AVM_RATE_IO_ACTIVE;

    st->current_rate = st->pending_rate;
    st->performed_mask |= (1u << device);
    if (st->performed_mask == all_mask(st)) {
        // Los tres dispositivos aplicaron la misma frecuencia: vuelven a estar coherentes.
        st->inconsistent = false;
        close_transaction(st);
    }
    return AVM_RATE_OK;
}

avm_rate_abort_result avm_rate_abort(avm_rate_state* st, uint32_t device, uint32_t generation, uint64_t now)
{
    avm_rate_abort_result r = { false, 0.0, 0 };
    (void)device;
    if (!avm_rate_pending(st) || generation != st->generation)
        return r;  // cancelación de una transacción ya resuelta: se ignora

    if (st->is_revert) {
        // La reversión también falló: no se insiste.
        st->inconsistent = true;
        close_transaction(st);
        return r;
    }

    if (st->performed_mask == 0) {
        // Nadie aplicó la nueva frecuencia: basta con cerrar.
        st->current_rate = st->previous_rate;
        close_transaction(st);
        return r;
    }

    // Algún dispositivo ya cambió: se revierte en todos a la frecuencia anterior.
    double back = st->previous_rate;
    next_generation(st);
    st->previous_rate = st->current_rate;
    st->pending_rate = back;
    st->performed_mask = 0;
    st->is_revert = true;
    st->started_at = now;
    r.revert_needed = true;
    r.revert_rate = back;
    r.revert_generation = st->generation;
    return r;
}

avm_rate_status avm_rate_io_start(avm_rate_state* st, uint32_t device, uint64_t now)
{
    if (device >= st->device_count) return AVM_RATE_STALE;
    expire_if_needed(st, now);
    if (avm_rate_pending(st)) return AVM_RATE_BUSY;
    st->running[device]++;
    st->running_total++;
    return AVM_RATE_OK;
}

void avm_rate_io_stop(avm_rate_state* st, uint32_t device)
{
    if (device >= st->device_count) return;
    if (st->running[device] > 0) {
        st->running[device]--;
        if (st->running_total > 0) st->running_total--;
    }
}
