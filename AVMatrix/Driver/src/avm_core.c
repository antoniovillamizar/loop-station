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

void avm_matrix_default(avm_matrix* m)
{
    avm_matrix_clear(m);
    for (int i = 0; i < AVM_CHANNELS_PER_PORT; i++) {
        m->cell[AVM_DST_LINK_BASE + i][AVM_SRC_SMAART_BASE + i] = 1;  // envío a Colombia
        m->cell[AVM_DST_SMAART_BASE + i][AVM_SRC_LINK_BASE + i] = 1;  // retorno a Smaart
    }
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

void avm_engine_init(avm_engine* e)
{
    memset(e->ring, 0, sizeof(e->ring));
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        atomic_init(&e->written_end[g], 0);
        atomic_init(&e->valid_begin[g], 0);
    }
    atomic_init(&e->matrix_seq, 0);
    avm_matrix_default(&e->matrix);
}

void avm_engine_reset(avm_engine* e)
{
    memset(e->ring, 0, sizeof(e->ring));
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        atomic_store_explicit(&e->written_end[g], 0, memory_order_release);
        atomic_store_explicit(&e->valid_begin[g], 0, memory_order_release);
    }
}

void avm_engine_set_matrix(avm_engine* e, const avm_matrix* m)
{
    // Un solo escritor a la vez (el driver lo protege con su mutex).
    uint32_t seq = atomic_load_explicit(&e->matrix_seq, memory_order_relaxed);
    atomic_store_explicit(&e->matrix_seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    memcpy(&e->matrix, m, sizeof(*m));
    atomic_store_explicit(&e->matrix_seq, seq + 2, memory_order_release);
}

void avm_engine_get_matrix(const avm_engine* e, avm_matrix* out)
{
    avm_engine* me = (avm_engine*)e;
    for (;;) {
        uint32_t s1 = atomic_load_explicit(&me->matrix_seq, memory_order_acquire);
        if (s1 & 1u) continue;
        memcpy(out, &me->matrix, sizeof(*out));
        atomic_thread_fence(memory_order_acquire);
        uint32_t s2 = atomic_load_explicit(&me->matrix_seq, memory_order_relaxed);
        if (s1 == s2) return;
    }
}

static void avm_zero_range(avm_engine* e, int base, int nch, int64_t from, int64_t to)
{
    if (to <= from) return;
    if (to - from >= AVM_RING_FRAMES) {
        for (int c = 0; c < nch; c++)
            memset(e->ring[base + c], 0, sizeof(e->ring[0]));
        return;
    }
    for (int64_t t = from; t < to; t++) {
        int64_t idx = t & AVM_RING_MASK;
        for (int c = 0; c < nch; c++)
            e->ring[base + c][idx] = 0.0f;
    }
}

void avm_engine_write(avm_engine* e, avm_src_group g, int64_t sample_time,
                      uint32_t frames, const void* interleaved, avm_format fmt,
                      uint32_t channels)
{
    if ((int)g < 0 || g >= AVM_SRC_GROUP_COUNT || frames == 0 || interleaved == NULL)
        return;
    const int base = avm_src_group_base(g);
    const int nch = avm_src_group_channels(g);
    const uint32_t bps = avm_format_bytes_per_sample(fmt);
    if (bps == 0 || channels == 0) return;
    if (frames > AVM_VALID_WINDOW) frames = AVM_VALID_WINDOW;

    const int64_t we = atomic_load_explicit(&e->written_end[g], memory_order_acquire);
    // Si hubo un hueco (la aplicación dejó de enviar), se borra para que el
    // lector nunca reciba audio viejo como si fuera nuevo.
    if (sample_time > we)
        avm_zero_range(e, base, nch, we, sample_time);

    const uint8_t* in = (const uint8_t*)interleaved;
    const uint32_t frame_bytes = bps * channels;
    for (uint32_t f = 0; f < frames; f++) {
        int64_t t = sample_time + (int64_t)f;
        if (t < 0) continue;
        int64_t idx = t & AVM_RING_MASK;
        const uint8_t* fp = in + (size_t)f * frame_bytes;
        for (int c = 0; c < nch; c++) {
            float v = ((uint32_t)c < channels) ? avm_sample_to_float(fp + (size_t)c * bps, fmt) : 0.0f;
            e->ring[base + c][idx] = v;
        }
    }

    int64_t end = sample_time + (int64_t)frames;
    if (end > we)
        atomic_store_explicit(&e->written_end[g], end, memory_order_release);
}

void avm_engine_read(avm_engine* e, avm_dst_group g, int64_t sample_time,
                     uint32_t frames, void* interleaved, avm_format fmt,
                     uint32_t channels)
{
    if ((int)g < 0 || g >= AVM_DST_GROUP_COUNT || frames == 0 || interleaved == NULL)
        return;
    const uint32_t bps = avm_format_bytes_per_sample(fmt);
    if (bps == 0 || channels == 0) return;

    avm_matrix m;
    avm_engine_get_matrix(e, &m);

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
    for (int d = 0; d < dnch; d++) {
        nsrc[d] = 0;
        for (int s = 0; s < AVM_NUM_SOURCES; s++)
            if (m.cell[dbase + d][s] && avm_route_allowed(dbase + d, s) == AVM_ROUTE_OK)
                srcs[d][nsrc[d]++] = s;
    }

    uint8_t* out = (uint8_t*)interleaved;
    const uint32_t frame_bytes = bps * channels;
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
                        acc += e->ring[s][idx];
                }
            }
            avm_float_to_sample(acc, fp + (size_t)c * bps, fmt);
        }
    }
}
