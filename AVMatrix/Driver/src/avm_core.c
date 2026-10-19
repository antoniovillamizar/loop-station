// AV Matrix — núcleo de transporte y enrutamiento. Ver avm_core.h.

#include "avm_core.h"

#include <math.h>
#include <string.h>

#define AVM_RING_MASK ((int64_t)AVM_RING_FRAMES - 1)

// ---------------------------------------------------------------------------
// Grupos

int avm_src_group_base(avm_src_group g)
{
    switch (g) {
        case AVM_SRC_GROUP_SMAART: return AVM_SRC_SMAART_BASE;
        case AVM_SRC_GROUP_LINK:   return AVM_SRC_LINK_BASE;
        case AVM_SRC_GROUP_MUSIC:  return AVM_SRC_MUSIC_BASE;
        default:                   return -1;
    }
}

int avm_src_group_channels(avm_src_group g)
{
    switch (g) {
        case AVM_SRC_GROUP_SMAART: return AVM_CHANNELS_PER_PORT;
        case AVM_SRC_GROUP_LINK:   return AVM_CHANNELS_PER_PORT;
        case AVM_SRC_GROUP_MUSIC:  return AVM_MUSIC_CHANNELS;
        default:                   return 0;
    }
}

int avm_dst_group_base(avm_dst_group g)
{
    switch (g) {
        case AVM_DST_GROUP_SMAART: return AVM_DST_SMAART_BASE;
        case AVM_DST_GROUP_LINK:   return AVM_DST_LINK_BASE;
        default:                   return -1;
    }
}

int avm_dst_group_channels(avm_dst_group g)
{
    switch (g) {
        case AVM_DST_GROUP_SMAART: return AVM_CHANNELS_PER_PORT;
        case AVM_DST_GROUP_LINK:   return AVM_CHANNELS_PER_PORT;
        default:                   return 0;
    }
}

avm_src_group avm_src_group_of(int src)
{
    if (src >= AVM_SRC_MUSIC_BASE) return AVM_SRC_GROUP_MUSIC;
    if (src >= AVM_SRC_LINK_BASE)  return AVM_SRC_GROUP_LINK;
    return AVM_SRC_GROUP_SMAART;
}

// ---------------------------------------------------------------------------
// Reglas

avm_route_status avm_route_allowed(int dst, int src)
{
    if (dst < 0 || dst >= AVM_NUM_DESTS || src < 0 || src >= AVM_NUM_SOURCES)
        return AVM_ROUTE_OUT_OF_RANGE;
    // El retorno que llega por LISTENTO nunca puede volver a enviarse por
    // LISTENTO: formaría un lazo de realimentación a través de Colombia.
    if (dst >= AVM_DST_LINK_BASE && src >= AVM_SRC_LINK_BASE && src < AVM_SRC_MUSIC_BASE)
        return AVM_ROUTE_FORBIDDEN_FEEDBACK;
    return AVM_ROUTE_OK;
}

void avm_matrix_clear(avm_matrix* m)
{
    memset(m, 0, sizeof(*m));
}

void avm_matrix_direct(avm_matrix* m)
{
    avm_matrix_clear(m);
    for (int i = 0; i < AVM_CHANNELS_PER_PORT; i++) {
        m->cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;  // envío a Colombia
        m->cell[AVM_DST_SMAART_BASE + i][AVM_SRC_LINK_BASE + i] = 1;  // retorno a Smaart
    }
}

void avm_matrix_default(avm_matrix* m)
{
    avm_matrix_direct(m);
    // Entrada 8 = referencia local: solo el generador, sin mezclar el retorno 8.
    m->cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_LINK_BASE + 7] = 0;
    m->cell[AVM_DST_LOCAL_REFERENCE][AVM_SRC_GENERATOR] = 1;
}

bool avm_matrix_is_valid(const avm_matrix* m)
{
    for (int d = 0; d < AVM_NUM_DESTS; d++)
        for (int s = 0; s < AVM_NUM_SOURCES; s++) {
            uint8_t v = m->cell[d][s];
            if (v > 1) return false;
            if (v && avm_route_allowed(d, s) != AVM_ROUTE_OK) return false;
        }
    return true;
}

// ---------------------------------------------------------------------------
// Formatos

uint32_t avm_format_bytes_per_sample(avm_format fmt)
{
    switch (fmt) {
        case AVM_FMT_FLOAT32: return 4;
        case AVM_FMT_SINT32:  return 4;
        case AVM_FMT_SINT24:  return 3;
        case AVM_FMT_SINT16:  return 2;
        default:              return 0;
    }
}

uint32_t avm_format_bits(avm_format fmt)
{
    switch (fmt) {
        case AVM_FMT_FLOAT32: return 32;
        case AVM_FMT_SINT32:  return 32;
        case AVM_FMT_SINT24:  return 24;
        case AVM_FMT_SINT16:  return 16;
        default:              return 0;
    }
}

float avm_sample_to_float(const void* src, avm_format fmt)
{
    const uint8_t* b = (const uint8_t*)src;
    switch (fmt) {
        case AVM_FMT_FLOAT32: {
            float v;
            memcpy(&v, b, 4);
            return isfinite(v) ? v : 0.0f;
        }
        case AVM_FMT_SINT32: {
            int32_t v;
            memcpy(&v, b, 4);
            return (float)((double)v / 2147483648.0);
        }
        case AVM_FMT_SINT24: {
            int32_t v = (int32_t)((uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16));
            if (v & 0x800000) v -= 0x1000000;
            return (float)v / 8388608.0f;
        }
        case AVM_FMT_SINT16: {
            int16_t v;
            memcpy(&v, b, 2);
            return (float)v / 32768.0f;
        }
        default:
            return 0.0f;
    }
}

static int64_t avm_clamp_round(double x, int64_t lo, int64_t hi)
{
    double r = nearbyint(x);
    if (r < (double)lo) return lo;
    if (r > (double)hi) return hi;
    return (int64_t)r;
}

void avm_float_to_sample(float v, void* dst, avm_format fmt)
{
    uint8_t* b = (uint8_t*)dst;
    if (!isfinite(v)) v = 0.0f;
    switch (fmt) {
        case AVM_FMT_FLOAT32:
            memcpy(b, &v, 4);
            break;
        case AVM_FMT_SINT32: {
            int32_t s = (int32_t)avm_clamp_round((double)v * 2147483648.0, INT32_MIN, INT32_MAX);
            memcpy(b, &s, 4);
            break;
        }
        case AVM_FMT_SINT24: {
            int32_t s = (int32_t)avm_clamp_round((double)v * 8388608.0, -8388608, 8388607);
            uint32_t u = (uint32_t)s;
            b[0] = (uint8_t)(u & 0xFF);
            b[1] = (uint8_t)((u >> 8) & 0xFF);
            b[2] = (uint8_t)((u >> 16) & 0xFF);
            break;
        }
        case AVM_FMT_SINT16: {
            int16_t s = (int16_t)avm_clamp_round((double)v * 32768.0, INT16_MIN, INT16_MAX);
            memcpy(b, &s, 2);
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Motor

static void avm_meter_init(avm_meter* m)
{
    atomic_init(&m->epoch, 0);
    atomic_init(&m->cur_peak, 0.0f);
    atomic_init(&m->prev_peak, 0.0f);
    atomic_init(&m->last_update_ns, 0);
    atomic_init(&m->last_clip_ns, 0);
}

// Un solo escritor por medidor: sin bucles ni esperas.
static void avm_meter_update(avm_meter* m, float peak, bool clip, uint64_t now_ns)
{
    uint64_t ep = now_ns / AVM_METER_EPOCH_NS;
    uint64_t cur_ep = atomic_load_explicit(&m->epoch, memory_order_relaxed);
    if (ep != cur_ep) {
        float carried = (ep == cur_ep + 1) ? atomic_load_explicit(&m->cur_peak, memory_order_relaxed) : 0.0f;
        atomic_store_explicit(&m->prev_peak, carried, memory_order_relaxed);
        atomic_store_explicit(&m->cur_peak, peak, memory_order_relaxed);
        atomic_store_explicit(&m->epoch, ep, memory_order_release);
    } else if (peak > atomic_load_explicit(&m->cur_peak, memory_order_relaxed)) {
        atomic_store_explicit(&m->cur_peak, peak, memory_order_relaxed);
    }
    if (clip) atomic_store_explicit(&m->last_clip_ns, now_ns, memory_order_relaxed);
    atomic_store_explicit(&m->last_update_ns, now_ns, memory_order_release);
}

static avm_meter_reading avm_meter_read(avm_meter* m, uint64_t now_ns)
{
    avm_meter_reading r = { 0.0f, false, false };
    uint64_t last = atomic_load_explicit(&m->last_update_ns, memory_order_acquire);
    uint64_t clip = atomic_load_explicit(&m->last_clip_ns, memory_order_relaxed);
    r.clip = clip != 0 && now_ns >= clip && now_ns - clip < AVM_METER_CLIP_HOLD_NS;
    r.active = last != 0 && now_ns >= last && now_ns - last < AVM_METER_ACTIVE_NS;
    if (!r.active) return r;  // sin bloques recientes: nunca se muestra un valor antiguo
    uint64_t ep_now = now_ns / AVM_METER_EPOCH_NS;
    uint64_t ep = atomic_load_explicit(&m->epoch, memory_order_acquire);
    float cur = atomic_load_explicit(&m->cur_peak, memory_order_relaxed);
    float prev = atomic_load_explicit(&m->prev_peak, memory_order_relaxed);
    if (ep == ep_now)          r.peak = cur > prev ? cur : prev;
    else if (ep + 1 == ep_now) r.peak = cur;
    return r;
}

static void avm_meter_clear(avm_meter* m)
{
    atomic_store_explicit(&m->last_update_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&m->last_clip_ns, 0, memory_order_relaxed);
    atomic_store_explicit(&m->cur_peak, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&m->prev_peak, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&m->epoch, 0, memory_order_relaxed);
}

void avm_engine_meter_snapshot(avm_engine* e, uint64_t now_ns,
                               avm_meter_reading src[AVM_NUM_SOURCES],
                               avm_meter_reading dst[AVM_NUM_DESTS])
{
    // Lado lector del seqlock de sesión: una sola pasada, sin reintentos.
    uint32_t s1 = atomic_load_explicit(&e->meter_session, memory_order_acquire);
    bool ok = (s1 & 1u) == 0;
    if (ok) {
        for (int i = 0; i < AVM_NUM_SOURCES; i++) src[i] = avm_meter_read(&e->src_meters[i], now_ns);
        for (int i = 0; i < AVM_NUM_DESTS; i++) dst[i] = avm_meter_read(&e->dst_meters[i], now_ns);
        atomic_thread_fence(memory_order_acquire);
        ok = atomic_load_explicit(&e->meter_session, memory_order_relaxed) == s1;
    }
    if (!ok) {
        // Reinicio en curso o recién hecho: todo inactivo, nunca valores de otra sesión.
        const avm_meter_reading none = { 0.0f, false, false };
        for (int i = 0; i < AVM_NUM_SOURCES; i++) src[i] = none;
        for (int i = 0; i < AVM_NUM_DESTS; i++) dst[i] = none;
    }
}

static void avm_put_u32(uint8_t* p, uint32_t v) { memcpy(p, &v, 4); }
static uint32_t avm_get_u32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }

void avm_meters_pack(const avm_meter_reading src[AVM_NUM_SOURCES],
                     const avm_meter_reading dst[AVM_NUM_DESTS], uint8_t out[AVM_METERS_BLOB_SIZE])
{
    memset(out, 0, AVM_METERS_BLOB_SIZE);
    avm_put_u32(out, 0x41564D31u);  // 'AVM1'
    avm_put_u32(out + 4, AVM_NUM_SOURCES);
    avm_put_u32(out + 8, AVM_NUM_DESTS);
    for (int i = 0; i < AVM_NUM_SOURCES + AVM_NUM_DESTS; i++) {
        const avm_meter_reading* r = i < AVM_NUM_SOURCES ? &src[i] : &dst[i - AVM_NUM_SOURCES];
        uint8_t* p = out + 16 + 8 * i;
        memcpy(p, &r->peak, 4);
        p[4] = r->active ? 1 : 0;
        p[5] = r->clip ? 1 : 0;
    }
}

bool avm_meters_unpack(const uint8_t* blob, size_t len,
                       avm_meter_reading src[AVM_NUM_SOURCES], avm_meter_reading dst[AVM_NUM_DESTS])
{
    if (blob == NULL || len != AVM_METERS_BLOB_SIZE) return false;
    if (avm_get_u32(blob) != 0x41564D31u || avm_get_u32(blob + 4) != AVM_NUM_SOURCES
        || avm_get_u32(blob + 8) != AVM_NUM_DESTS)
        return false;
    for (int i = 0; i < AVM_NUM_SOURCES + AVM_NUM_DESTS; i++) {
        avm_meter_reading* r = i < AVM_NUM_SOURCES ? &src[i] : &dst[i - AVM_NUM_SOURCES];
        const uint8_t* p = blob + 16 + 8 * i;
        memcpy(&r->peak, p, 4);
        if (!isfinite(r->peak) || r->peak < 0.0f) r->peak = 0.0f;
        r->active = p[4] != 0;
        r->clip = p[5] != 0;
    }
    return true;
}

bool avm_format_is_full_scale(float hi, float lo, avm_format fmt)
{
    // Ida y vuelta con la conversión de entrega: el resultado es exactamente
    // código/escala, comparable con los extremos del formato.
    return avm_quantize(hi, fmt) >= avm_format_clip_level(fmt)
        || avm_quantize(lo, fmt) <= -1.0f;
}

float avm_quantize(float v, avm_format fmt)
{
    uint8_t b[4];
    avm_float_to_sample(v, b, fmt);
    return avm_sample_to_float(b, fmt);
}

float avm_format_clip_level(avm_format fmt)
{
    switch (fmt) {
        case AVM_FMT_SINT24: return 8388607.0f / 8388608.0f;
        case AVM_FMT_SINT16: return 32767.0f / 32768.0f;
        case AVM_FMT_SINT32:  // 2147483647/2^31 se representa como 1.0 en float
        case AVM_FMT_FLOAT32:
        default:             return 1.0f;
    }
}

float avm_peak_to_dbfs(float peak)
{
    if (!(peak > 0.0f)) return -INFINITY;
    return 20.0f * log10f(peak);
}

// ---------------------------------------------------------------------------
// Mixer

void avm_mixer_default(avm_mixer* mx)
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++) { mx->src_gain[i] = 1.0f; mx->src_mute[i] = 0; }
    for (int i = 0; i < AVM_NUM_DESTS; i++)   { mx->dst_gain[i] = 1.0f; mx->dst_mute[i] = 0; }
}

static bool avm_gain_ok(float g) { return isfinite(g) && g >= 0.0f && g <= 1.0f; }

bool avm_mixer_is_valid(const avm_mixer* mx)
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++)
        if (!avm_gain_ok(mx->src_gain[i]) || mx->src_mute[i] > 1) return false;
    for (int i = 0; i < AVM_NUM_DESTS; i++)
        if (!avm_gain_ok(mx->dst_gain[i]) || mx->dst_mute[i] > 1) return false;
    return true;
}

bool avm_mixer_is_unity(const avm_mixer* mx)
{
    for (int i = 0; i < AVM_NUM_SOURCES; i++)
        if (mx->src_gain[i] != 1.0f || mx->src_mute[i]) return false;
    for (int i = 0; i < AVM_NUM_DESTS; i++)
        if (mx->dst_gain[i] != 1.0f || mx->dst_mute[i]) return false;
    return true;
}

float avm_mixer_src_effective(const avm_mixer* mx, int src)
{
    if (src < 0 || src >= AVM_NUM_SOURCES) return 0.0f;
    return mx->src_mute[src] ? 0.0f : mx->src_gain[src];
}

float avm_mixer_dst_effective(const avm_mixer* mx, int dst)
{
    if (dst < 0 || dst >= AVM_NUM_DESTS) return 0.0f;
    return mx->dst_mute[dst] ? 0.0f : mx->dst_gain[dst];
}

float avm_db_to_gain(float db)
{
    if (isnan(db)) return 0.0f;
    if (db >= 0.0f) return 1.0f;            // sin amplificación; 0 dB = 1 exacto
    if (isinf(db)) return 0.0f;             // -INFINITY = silencio
    float g = powf(10.0f, db / 20.0f);
    return g > 1.0f ? 1.0f : g;
}

float avm_gain_to_db(float gain)
{
    if (!(gain > 0.0f)) return -INFINITY;
    if (gain >= 1.0f) return 0.0f;
    return 20.0f * log10f(gain);
}

void avm_mixer_pack(const avm_mixer* mx, uint8_t out[AVM_MIXER_BLOB_SIZE])
{
    memset(out, 0, AVM_MIXER_BLOB_SIZE);
    memcpy(out, "AVX1", 4);
    avm_put_u32(out + 4, AVM_NUM_SOURCES);
    avm_put_u32(out + 8, AVM_NUM_DESTS);
    uint8_t* p = out + 16;
    for (int i = 0; i < AVM_NUM_SOURCES; i++, p += 8) { memcpy(p, &mx->src_gain[i], 4); p[4] = mx->src_mute[i]; }
    for (int i = 0; i < AVM_NUM_DESTS; i++, p += 8)   { memcpy(p, &mx->dst_gain[i], 4); p[4] = mx->dst_mute[i]; }
}

bool avm_mixer_unpack(const uint8_t* blob, size_t len, avm_mixer* mx)
{
    if (blob == NULL || len != AVM_MIXER_BLOB_SIZE || memcmp(blob, "AVX1", 4) != 0
        || avm_get_u32(blob + 4) != AVM_NUM_SOURCES || avm_get_u32(blob + 8) != AVM_NUM_DESTS
        || avm_get_u32(blob + 12) != 0)
        return false;
    avm_mixer t;
    const uint8_t* p = blob + 16;
    for (int i = 0; i < AVM_NUM_SOURCES; i++, p += 8) {
        memcpy(&t.src_gain[i], p, 4); t.src_mute[i] = p[4];
        if (p[5] || p[6] || p[7]) return false;
    }
    for (int i = 0; i < AVM_NUM_DESTS; i++, p += 8) {
        memcpy(&t.dst_gain[i], p, 4); t.dst_mute[i] = p[4];
        if (p[5] || p[6] || p[7]) return false;
    }
    if (!avm_mixer_is_valid(&t)) return false;
    *mx = t;
    return true;
}

static void avm_gain_init(avm_gain* g, float v)
{
    atomic_init(&g->target, v);
    g->cur = g->ramp_to = v;
    g->step = 0.0f;
    g->remaining = 0;
}

// Fin de las rampas (sin IO activo): el canal queda en su objetivo.
static void avm_gain_settle(avm_gain* g)
{
    float t = atomic_load_explicit(&g->target, memory_order_relaxed);
    g->cur = g->ramp_to = t;
    g->step = 0.0f;
    g->remaining = 0;
}

// Al principio de cada bloque (hilo de IO dueño del canal): si el objetivo
// cambió, empieza una rampa desde el nivel actual. Una sola carga atómica.
static inline void avm_gain_begin_block(avm_gain* g)
{
    float t = atomic_load_explicit(&g->target, memory_order_relaxed);
    if (t != g->ramp_to) {
        g->ramp_to = t;
        g->step = (t - g->cur) / (float)AVM_GAIN_RAMP_FRAMES;
        g->remaining = AVM_GAIN_RAMP_FRAMES;
    }
}

// Ganancia de la muestra siguiente (avanza la rampa una muestra).
static inline float avm_gain_next(avm_gain* g)
{
    if (g->remaining) {
        g->cur += g->step;
        // El redondeo acumulado nunca debe pasar del objetivo (ni amplificar).
        if ((g->step > 0.0f && g->cur > g->ramp_to) || (g->step < 0.0f && g->cur < g->ramp_to))
            g->cur = g->ramp_to;
        if (--g->remaining == 0) g->cur = g->ramp_to;   // termina exactamente en el objetivo
    }
    return g->cur;
}

// Aplica una ganancia. A 1 la muestra no se toca (idéntica bit a bit, incluso
// -0 y NaN, como antes del mixer); a 0 se entrega silencio exacto.
static inline float avm_gain_apply(float v, float gain)
{
    if (gain == 1.0f) return v;
    if (gain == 0.0f) return 0.0f;
    return v * gain;
}

static void avm_ring_zero_all(avm_engine* e)
{
    for (int s = 0; s < AVM_NUM_SOURCES; s++)
        for (int i = 0; i < AVM_RING_FRAMES; i++)
            atomic_store_explicit(&e->ring[s][i], 0.0f, memory_order_relaxed);
}

static void avm_slot_write(avm_matrix_slot* slot, const avm_matrix* m)
{
    uint64_t words[AVM_MATRIX_WORDS] = { 0 };
    memcpy(words, m->cell, sizeof(m->cell));
    uint32_t seq = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    atomic_store_explicit(&slot->seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (size_t i = 0; i < AVM_MATRIX_WORDS; i++)
        atomic_store_explicit(&slot->words[i], words[i], memory_order_relaxed);
    atomic_store_explicit(&slot->seq, seq + 2, memory_order_release);
}

// Un intento de lectura de una ranura. Devuelve false si la ranura cambió durante la copia.
static bool avm_slot_read(avm_matrix_slot* slot, avm_matrix* out)
{
    uint32_t s1 = atomic_load_explicit(&slot->seq, memory_order_acquire);
    if (s1 & 1u) return false;
    uint64_t words[AVM_MATRIX_WORDS];
    for (size_t i = 0; i < AVM_MATRIX_WORDS; i++)
        words[i] = atomic_load_explicit(&slot->words[i], memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    uint32_t s2 = atomic_load_explicit(&slot->seq, memory_order_relaxed);
    if (s1 != s2) return false;
    memcpy(out->cell, words, sizeof(out->cell));
    return true;
}

void avm_engine_init(avm_engine* e)
{
    avm_ring_zero_all(e);
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        atomic_init(&e->written_end[g], 0);
        atomic_init(&e->valid_begin[g], 0);
        atomic_init(&e->writing_end[g], 0);
    }
    atomic_init(&e->overrun_reads, 0);
    avm_matrix m;
    avm_matrix_default(&m);
    for (int i = 0; i < AVM_MATRIX_SLOTS; i++) {
        atomic_init(&e->matrix_slots[i].seq, 0);
        for (size_t w = 0; w < AVM_MATRIX_WORDS; w++)
            atomic_init(&e->matrix_slots[i].words[w], 0);
        avm_slot_write(&e->matrix_slots[i], &m);
    }
    atomic_init(&e->matrix_published, 0);
    for (int g = 0; g < AVM_DST_GROUP_COUNT; g++)
        e->reader_cache[g] = m;
    atomic_init(&e->stale_matrix_reads, 0);
    for (int i = 0; i < AVM_NUM_SOURCES; i++) avm_meter_init(&e->src_meters[i]);
    for (int i = 0; i < AVM_NUM_DESTS; i++) avm_meter_init(&e->dst_meters[i]);
    atomic_init(&e->meter_session, 0);
    avm_mixer_default(&e->mixer);
    for (int i = 0; i < AVM_NUM_SOURCES; i++) avm_gain_init(&e->src_gain[i], 1.0f);
    for (int i = 0; i < AVM_NUM_DESTS; i++) avm_gain_init(&e->dst_gain[i], 1.0f);
}

void avm_engine_reset(avm_engine* e)
{
    avm_ring_zero_all(e);
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        atomic_store_explicit(&e->written_end[g], 0, memory_order_release);
        atomic_store_explicit(&e->valid_begin[g], 0, memory_order_release);
        atomic_store_explicit(&e->writing_end[g], 0, memory_order_release);
    }

    // Medidores: lado escritor del seqlock de sesión (impar durante la limpieza).
    uint32_t sess = atomic_load_explicit(&e->meter_session, memory_order_relaxed);
    atomic_store_explicit(&e->meter_session, sess + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    for (int i = 0; i < AVM_NUM_SOURCES; i++) avm_meter_clear(&e->src_meters[i]);
    for (int i = 0; i < AVM_NUM_DESTS; i++) avm_meter_clear(&e->dst_meters[i]);
    atomic_store_explicit(&e->meter_session, sess + 2, memory_order_release);

    for (int i = 0; i < AVM_NUM_SOURCES; i++) avm_gain_settle(&e->src_gain[i]);
    for (int i = 0; i < AVM_NUM_DESTS; i++) avm_gain_settle(&e->dst_gain[i]);
}

bool avm_engine_set_mixer(avm_engine* e, const avm_mixer* mx)
{
    if (mx == NULL || !avm_mixer_is_valid(mx)) return false;
    e->mixer = *mx;
    for (int i = 0; i < AVM_NUM_SOURCES; i++)
        atomic_store_explicit(&e->src_gain[i].target, avm_mixer_src_effective(mx, i), memory_order_relaxed);
    for (int i = 0; i < AVM_NUM_DESTS; i++)
        atomic_store_explicit(&e->dst_gain[i].target, avm_mixer_dst_effective(mx, i), memory_order_relaxed);
    return true;
}

void avm_engine_get_mixer(const avm_engine* e, avm_mixer* out)
{
    *out = e->mixer;
}

void avm_engine_set_matrix(avm_engine* e, const avm_matrix* m)
{
    // Se escribe siempre en una ranura distinta de la publicada: un escritor
    // detenido a mitad de escritura no afecta a la ranura que leen los lectores.
    uint32_t cur = atomic_load_explicit(&e->matrix_published, memory_order_relaxed);
    uint32_t next = (cur + 1) % AVM_MATRIX_SLOTS;
    avm_slot_write(&e->matrix_slots[next], m);
    atomic_store_explicit(&e->matrix_published, next, memory_order_release);
}

bool avm_engine_try_get_matrix(avm_engine* e, avm_matrix* out)
{
    for (int attempt = 0; attempt < AVM_MATRIX_READ_ATTEMPTS; attempt++) {
        uint32_t idx = atomic_load_explicit(&e->matrix_published, memory_order_acquire);
        if (idx >= AVM_MATRIX_SLOTS) return false;
        if (avm_slot_read(&e->matrix_slots[idx], out)) return true;
    }
    return false;
}

static void avm_zero_range(avm_engine* e, int base, int nch, int64_t from, int64_t to)
{
    if (to <= from) return;
    if (to - from > AVM_RING_FRAMES) from = to - AVM_RING_FRAMES;
    for (int64_t t = from; t < to; t++) {
        int64_t idx = t & AVM_RING_MASK;
        for (int c = 0; c < nch; c++)
            atomic_store_explicit(&e->ring[base + c][idx], 0.0f, memory_order_relaxed);
    }
}

void avm_engine_write(avm_engine* e, avm_src_group g, int64_t sample_time,
                      uint32_t frames, const void* interleaved, avm_format fmt,
                      uint32_t channels)
{
    avm_engine_write_metered(e, g, sample_time, frames, interleaved, fmt, channels, 0);
}

void avm_engine_write_metered(avm_engine* e, avm_src_group g, int64_t sample_time,
                              uint32_t frames, const void* interleaved, avm_format fmt,
                              uint32_t channels, uint64_t now_ns)
{
    if ((int)g < 0 || g >= AVM_SRC_GROUP_COUNT || frames == 0 || interleaved == NULL)
        return;
    const int base = avm_src_group_base(g);
    const int nch = avm_src_group_channels(g);
    const uint32_t bps = avm_format_bytes_per_sample(fmt);
    if (bps == 0 || channels == 0) return;
    if (frames > AVM_VALID_WINDOW) frames = AVM_VALID_WINDOW;

    const int64_t we = atomic_load_explicit(&e->written_end[g], memory_order_acquire);
    const int64_t end = sample_time + (int64_t)frames;

    // Publicar hasta dónde se va a escribir antes de tocar el buffer
    // (contador de seqlock: almacenamiento + barrera de liberación).
    if (end > atomic_load_explicit(&e->writing_end[g], memory_order_relaxed))
        atomic_store_explicit(&e->writing_end[g], end, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    // Si hubo un hueco (la aplicación dejó de enviar), se borra para que el
    // lector nunca reciba audio viejo como si fuera nuevo.
    if (sample_time > we)
        avm_zero_range(e, base, nch, we, sample_time);

    const uint8_t* in = (const uint8_t*)interleaved;
    const uint32_t frame_bytes = bps * channels;
    float vmax[AVM_CHANNELS_PER_PORT] = { 0 }, vmin[AVM_CHANNELS_PER_PORT] = { 0 };
    // Volumen de entrada del mixer (antes de la matriz). Este hilo es el único
    // escritor del grupo: es el dueño de la rampa de sus canales.
    avm_gain* gains = &e->src_gain[base];
    for (int c = 0; c < nch; c++) avm_gain_begin_block(&gains[c]);
    for (uint32_t f = 0; f < frames; f++) {
        int64_t t = sample_time + (int64_t)f;
        float gf[AVM_CHANNELS_PER_PORT];
        for (int c = 0; c < nch; c++) gf[c] = avm_gain_next(&gains[c]);
        if (t < 0) continue;
        int64_t idx = t & AVM_RING_MASK;
        const uint8_t* fp = in + (size_t)f * frame_bytes;
        for (int c = 0; c < nch; c++) {
            float v = ((uint32_t)c < channels) ? avm_sample_to_float(fp + (size_t)c * bps, fmt) : 0.0f;
            v = avm_gain_apply(v, gf[c]);
            atomic_store_explicit(&e->ring[base + c][idx], v, memory_order_relaxed);
            // Medición: solo se observa el valor ya convertido (extremos con signo).
            if (v > vmax[c]) vmax[c] = v;
            if (v < vmin[c]) vmin[c] = v;
        }
    }

    if (end > we)
        atomic_store_explicit(&e->written_end[g], end, memory_order_release);

    if (now_ns != 0) {
        for (int c = 0; c < nch; c++)
            avm_meter_update(&e->src_meters[base + c], vmax[c] > -vmin[c] ? vmax[c] : -vmin[c],
                             avm_format_is_full_scale(vmax[c], vmin[c], fmt), now_ns);
    }
}

bool avm_engine_read(avm_engine* e, avm_dst_group g, int64_t sample_time,
                     uint32_t frames, void* interleaved, avm_format fmt,
                     uint32_t channels)
{
    return avm_engine_read_metered(e, g, sample_time, frames, interleaved, fmt, channels, 0);
}

bool avm_engine_read_metered(avm_engine* e, avm_dst_group g, int64_t sample_time,
                             uint32_t frames, void* interleaved, avm_format fmt,
                             uint32_t channels, uint64_t now_ns)
{
    if ((int)g < 0 || g >= AVM_DST_GROUP_COUNT || frames == 0 || interleaved == NULL)
        return true;
    const uint32_t bps = avm_format_bytes_per_sample(fmt);
    if (bps == 0 || channels == 0) return true;

    // Lectura acotada de la matriz; si no hay copia coherente se usa la última válida.
    avm_matrix* m_cache = &e->reader_cache[g];
    if (!avm_engine_try_get_matrix(e, m_cache))
        atomic_fetch_add_explicit(&e->stale_matrix_reads, 1, memory_order_relaxed);
    const avm_matrix* mp = m_cache;

    int64_t lo[AVM_SRC_GROUP_COUNT], hi[AVM_SRC_GROUP_COUNT];
    for (int sg = 0; sg < AVM_SRC_GROUP_COUNT; sg++) {
        int64_t we = atomic_load_explicit(&e->written_end[sg], memory_order_acquire);
        int64_t vb = atomic_load_explicit(&e->valid_begin[sg], memory_order_acquire);
        int64_t l = we - AVM_VALID_WINDOW;
        lo[sg] = (l > vb) ? l : vb;
        if (lo[sg] < 0) lo[sg] = 0;
        hi[sg] = we;
    }

    const int dbase = avm_dst_group_base(g);
    const int dnch = avm_dst_group_channels(g);

    // Lista de fuentes conectadas a cada destino de este grupo.
    int srcs[AVM_CHANNELS_PER_PORT][AVM_NUM_SOURCES];
    int nsrc[AVM_CHANNELS_PER_PORT];
    bool group_used[AVM_SRC_GROUP_COUNT] = { false };
    for (int d = 0; d < dnch; d++) {
        nsrc[d] = 0;
        for (int s = 0; s < AVM_NUM_SOURCES; s++)
            if (mp->cell[dbase + d][s] && avm_route_allowed(dbase + d, s) == AVM_ROUTE_OK) {
                srcs[d][nsrc[d]++] = s;
                group_used[avm_src_group_of(s)] = true;
            }
    }

    uint8_t* out = (uint8_t*)interleaved;
    const uint32_t frame_bytes = bps * channels;
    float vmax[AVM_CHANNELS_PER_PORT] = { 0 }, vmin[AVM_CHANNELS_PER_PORT] = { 0 };
    // Volumen de salida del mixer (después del enrutamiento y la suma). Este
    // hilo es el único lector del grupo: es el dueño de la rampa de sus canales.
    avm_gain* gains = &e->dst_gain[dbase];
    for (int d = 0; d < dnch; d++) avm_gain_begin_block(&gains[d]);
    for (uint32_t f = 0; f < frames; f++) {
        int64_t t = sample_time + (int64_t)f;
        int64_t idx = t & AVM_RING_MASK;
        uint8_t* fp = out + (size_t)f * frame_bytes;
        for (uint32_t c = 0; c < channels; c++) {
            float acc = 0.0f;
            if ((int)c < dnch) {
                for (int k = 0; k < nsrc[c]; k++) {
                    int s = srcs[c][k];
                    int sg = (int)avm_src_group_of(s);
                    if (t >= lo[sg] && t < hi[sg])
                        acc += atomic_load_explicit(&e->ring[s][idx], memory_order_relaxed);
                }
                acc = avm_gain_apply(acc, avm_gain_next(&gains[c]));
            }
            avm_float_to_sample(acc, fp + (size_t)c * bps, fmt);
            if ((int)c < dnch) {
                // Medición: el valor sumado que se entrega (extremos con signo).
                if (acc > vmax[c]) vmax[c] = acc;
                if (acc < vmin[c]) vmin[c] = acc;
            }
        }
    }

    // Validación posterior (lado lector del seqlock): si mientras se leía el
    // escritor llegó a escribir el instante t + AVM_RING_FRAMES de alguna fuente
    // usada, las muestras leídas pueden ser de otra vuelta del buffer.
    atomic_thread_fence(memory_order_acquire);
    for (int sg = 0; sg < AVM_SRC_GROUP_COUNT; sg++) {
        if (!group_used[sg]) continue;
        int64_t wb = atomic_load_explicit(&e->writing_end[sg], memory_order_relaxed);
        if (wb - (int64_t)AVM_RING_FRAMES > sample_time) {
            memset(out, 0, (size_t)frames * frame_bytes);  // cero en todos los formatos
            atomic_fetch_add_explicit(&e->overrun_reads, 1, memory_order_relaxed);
            if (now_ns != 0)
                for (int c = 0; c < dnch; c++)  // se entregó silencio: eso se mide
                    avm_meter_update(&e->dst_meters[dbase + c], 0.0f, false, now_ns);
            return false;
        }
    }
    if (now_ns != 0) {
        // Saturación según la conversión real al formato entregado: se enciende
        // exactamente cuando se entrega el código máximo o el mínimo (una suma
        // de 1,0 o más siempre lo produce).
        for (int c = 0; c < dnch; c++)
            avm_meter_update(&e->dst_meters[dbase + c], vmax[c] > -vmin[c] ? vmax[c] : -vmin[c],
                             avm_format_is_full_scale(vmax[c], vmin[c], fmt), now_ns);
    }
    return true;
}
