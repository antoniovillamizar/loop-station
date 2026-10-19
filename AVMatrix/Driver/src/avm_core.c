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
}

void avm_engine_reset(avm_engine* e)
{
    avm_ring_zero_all(e);
    for (int g = 0; g < AVM_SRC_GROUP_COUNT; g++) {
        atomic_store_explicit(&e->written_end[g], 0, memory_order_release);
        atomic_store_explicit(&e->valid_begin[g], 0, memory_order_release);
        atomic_store_explicit(&e->writing_end[g], 0, memory_order_release);
    }
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
    for (uint32_t f = 0; f < frames; f++) {
        int64_t t = sample_time + (int64_t)f;
        if (t < 0) continue;
        int64_t idx = t & AVM_RING_MASK;
        const uint8_t* fp = in + (size_t)f * frame_bytes;
        for (int c = 0; c < nch; c++) {
            float v = ((uint32_t)c < channels) ? avm_sample_to_float(fp + (size_t)c * bps, fmt) : 0.0f;
            atomic_store_explicit(&e->ring[base + c][idx], v, memory_order_relaxed);
        }
    }

    if (end > we)
        atomic_store_explicit(&e->written_end[g], end, memory_order_release);
}

bool avm_engine_read(avm_engine* e, avm_dst_group g, int64_t sample_time,
                     uint32_t frames, void* interleaved, avm_format fmt,
                     uint32_t channels)
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
            }
            avm_float_to_sample(acc, fp + (size_t)c * bps, fmt);
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
            return false;
        }
    }
    return true;
}
