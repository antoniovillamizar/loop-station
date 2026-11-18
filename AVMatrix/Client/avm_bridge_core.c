// AV Matrix — lógica de los puentes Dante y EVO. Ver avm_bridge_core.h.

#include "avm_bridge_core.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int avm_bridge_channels(avm_bridge_kind kind)
{
    return kind == AVM_BRIDGE_DANTE ? 2 : 4;
}

void avm_bridge_config_default(avm_bridge_config* c, avm_bridge_kind kind)
{
    memset(c, 0, sizeof(*c));
    c->kind = kind;
    for (int k = 0; k < avm_bridge_channels(kind); k++) c->channel[k] = k + 1;   // Dante 1-2, EVO 1-4
}

const char* avm_bridge_state_label(avm_bridge_state s)
{
    switch (s) {
        case AVM_BRIDGE_OFF:            return "Detenido";
        case AVM_BRIDGE_RUN:            return "Conectado";
        case AVM_BRIDGE_WAIT_DEVICE:    return "Desconectado";
        case AVM_BRIDGE_WAIT_AVMATRIX:  return "Falta AV Matrix";
        case AVM_BRIDGE_RATE_MISMATCH:  return "Frecuencias distintas";
        case AVM_BRIDGE_BAD_CONFIG:     return "Configuración no válida";
        case AVM_BRIDGE_SUSPENDED:      return "En pausa";
        case AVM_BRIDGE_ERROR:          return "Error";
    }
    return "?";
}

static const char* chname(avm_bridge_kind kind, int k, char* b, size_t len)
{
    if (kind == AVM_BRIDGE_DANTE) snprintf(b, len, "Dante %s", k == 0 ? "L" : "R");
    else snprintf(b, len, "EVO %d", k + 1);
    return b;
}

avm_bridge_state avm_bridge_evaluate(const avm_bridge_config* c, const avm_bridge_env* env, char* msg, size_t len)
{
    const bool dante = c->kind == AVM_BRIDGE_DANTE;
    const char* what = dante ? "Dante Virtual Soundcard" : "la EVO";
    const int n = avm_bridge_channels(c->kind);
    char a[32], b[32];
    if (len) msg[0] = 0;

    if (!c->enabled) { snprintf(msg, len, "Detenido. Pulse «Activar» para %s.", dante ? "enviar a Dante" : "recibir la EVO"); return AVM_BRIDGE_OFF; }
    if (env->suspended) { snprintf(msg, len, "En pausa mientras AV Matrix cambia de frecuencia."); return AVM_BRIDGE_SUSPENDED; }
    if (!env->avm_present) {
        snprintf(msg, len, "Falta «AV Matrix %s (puente)»: instale el driver de esta versión.", dante ? "Dante" : "EVO");
        return AVM_BRIDGE_WAIT_AVMATRIX;
    }
    if (c->device_uid[0] == 0) { snprintf(msg, len, "Elija el dispositivo de %s.", dante ? "salida (Dante Virtual Soundcard)" : "entrada (la EVO)"); return AVM_BRIDGE_BAD_CONFIG; }
    if (env->ext_is_avmatrix) {
        snprintf(msg, len, "No se puede usar un dispositivo de AV Matrix: crearía realimentación.");
        return AVM_BRIDGE_BAD_CONFIG;
    }
    if (env->ext_used_by_other) {
        snprintf(msg, len, "El otro puente ya usa este dispositivo: crearía realimentación. Elija otro.");
        return AVM_BRIDGE_BAD_CONFIG;
    }
    if (!env->ext_present) {
        snprintf(msg, len, "%s desconectada: %s. Se reconectará sola al volver, sin audio viejo.",
                 dante ? "Dante Virtual Soundcard" : "La EVO", dante ? "envío detenido" : "recepción detenida");
        return AVM_BRIDGE_WAIT_DEVICE;
    }
    // Canales: válidos, sin repetir; Dante necesita L y R dentro de lo que expone DVS.
    int used = 0, missing = 0;
    for (int k = 0; k < n; k++) {
        int ch = c->channel[k];
        if (ch < 0) { snprintf(msg, len, "Canal no válido para %s.", chname(c->kind, k, a, sizeof(a))); return AVM_BRIDGE_BAD_CONFIG; }
        if (ch == 0) {
            if (dante) { snprintf(msg, len, "Elija un canal de DVS para %s.", chname(c->kind, k, a, sizeof(a))); return AVM_BRIDGE_BAD_CONFIG; }
            continue;
        }
        for (int j = 0; j < k; j++)
            if (c->channel[j] == ch) {
                snprintf(msg, len, "%s y %s usan el mismo canal %d: los canales deben estar separados.",
                         chname(c->kind, j, a, sizeof(a)), chname(c->kind, k, b, sizeof(b)), ch);
                return AVM_BRIDGE_BAD_CONFIG;
            }
        if ((uint32_t)ch > env->ext_channels) {
            if (dante) {
                snprintf(msg, len, "%s solo tiene %u salidas: el canal %d no existe.", env->ext_name[0] ? env->ext_name : what,
                         (unsigned)env->ext_channels, ch);
                return AVM_BRIDGE_BAD_CONFIG;
            }
            missing++;
            continue;
        }
        used++;
    }
    if (used == 0) {
        snprintf(msg, len, "%s no expone ninguno de los canales elegidos (tiene %u entradas).",
                 env->ext_name[0] ? env->ext_name : what, (unsigned)env->ext_channels);
        return AVM_BRIDGE_BAD_CONFIG;
    }
    if (!(env->avm_rate > 0) || !(env->ext_rate > 0) || fabs(env->avm_rate - env->ext_rate) > 0.5) {
        snprintf(msg, len, "Frecuencias distintas: AV Matrix %.1f kHz, %s %.1f kHz. Iguálelas para %s.",
                 env->avm_rate / 1000.0, env->ext_name[0] ? env->ext_name : what, env->ext_rate / 1000.0,
                 dante ? "enviar" : "recibir");
        return AVM_BRIDGE_RATE_MISMATCH;
    }
    int loop = 0;
    if (!dante && env->loopback_from > 0)
        for (int k = 0; k < n; k++) if (c->channel[k] >= (int)env->loopback_from) loop = c->channel[k];
    if (missing)
        snprintf(msg, len, "Conectado a %s a %.1f kHz. Aviso: solo expone %u entradas; %d canal(es) en silencio.",
                 env->ext_name, env->ext_rate / 1000.0, (unsigned)env->ext_channels, missing);
    else if (loop)
        snprintf(msg, len, "Conectado a %s a %.1f kHz. Aviso: el canal %d es loopback (lo que el Mac reproduce en la EVO), no una entrada física.",
                 env->ext_name, env->ext_rate / 1000.0, loop);
    else if (dante)
        snprintf(msg, len, "Conectado a %s, canales %d y %d, a %.1f kHz.", env->ext_name, c->channel[0], c->channel[1], env->ext_rate / 1000.0);
    else
        snprintf(msg, len, "Conectado a %s (%u entradas) a %.1f kHz.", env->ext_name, (unsigned)env->ext_channels, env->ext_rate / 1000.0);
    return AVM_BRIDGE_RUN;
}

bool avm_bridge_locate(const uint32_t* buf_ch, int nbufs, uint32_t global, int* buf, int* ch)
{
    uint32_t base = 0;
    for (int b = 0; b < nbufs; b++) {
        if (global < base + buf_ch[b]) { *buf = b; *ch = (int)(global - base); return true; }
        base += buf_ch[b];
    }
    return false;
}

static uint32_t sum_ch(const uint32_t* buf_ch, int n)
{
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += buf_ch[i];
    return s;
}

bool avm_bridge_make_plan(const avm_bridge_config* c,
                          const uint32_t* in_buf_ch, int in_nbufs, const uint32_t* out_buf_ch, int out_nbufs,
                          uint32_t avm_in, uint32_t avm_out, uint32_t ext_in, uint32_t ext_out,
                          avm_bridge_plan* p, char* why, size_t len)
{
    memset(p, 0, sizeof(*p));
    if (len) why[0] = 0;
    if (in_nbufs < 0 || out_nbufs < 0 || in_nbufs > AVM_BRIDGE_MAX_BUFS || out_nbufs > AVM_BRIDGE_MAX_BUFS) {
        snprintf(why, len, "demasiados streams en el dispositivo agregado");
        return false;
    }
    if (sum_ch(in_buf_ch, in_nbufs) != avm_in + ext_in || sum_ch(out_buf_ch, out_nbufs) != avm_out + ext_out) {
        snprintf(why, len, "el dispositivo agregado no tiene los canales esperados (%u+%u entradas, %u+%u salidas)",
                 (unsigned)avm_in, (unsigned)ext_in, (unsigned)avm_out, (unsigned)ext_out);
        return false;
    }
    const bool dante = c->kind == AVM_BRIDGE_DANTE;
    const int n = avm_bridge_channels(c->kind);
    if ((dante && avm_in < (uint32_t)n) || (!dante && avm_out < (uint32_t)n)) {
        snprintf(why, len, "el dispositivo puente de AV Matrix no tiene %d canales", n);
        return false;
    }
    p->in_nbufs = in_nbufs;
    p->out_nbufs = out_nbufs;
    p->avm_in = avm_in; p->avm_out = avm_out; p->ext_in = ext_in; p->ext_out = ext_out;
    atomic_init(&p->shape_mismatch, 0);
    memcpy(p->in_buf_ch, in_buf_ch, sizeof(uint32_t) * (size_t)in_nbufs);
    memcpy(p->out_buf_ch, out_buf_ch, sizeof(uint32_t) * (size_t)out_nbufs);
    for (int k = 0; k < n; k++) {
        int ch = c->channel[k];
        if (ch <= 0) continue;
        uint32_t gin, gout;
        if (dante) {          // AV Matrix Dante (entradas 0-1) -> salida ch de DVS
            if ((uint32_t)ch > ext_out) continue;
            gin = (uint32_t)k;
            gout = avm_out + (uint32_t)(ch - 1);
        } else {              // entrada ch de la EVO -> AV Matrix EVO (salidas 0-3)
            if ((uint32_t)ch > ext_in) continue;   // canal que la EVO no expone: silencio
            gin = avm_in + (uint32_t)(ch - 1);
            gout = (uint32_t)k;
        }
        int ib, ic, ob, oc;
        if (!avm_bridge_locate(in_buf_ch, in_nbufs, gin, &ib, &ic) || !avm_bridge_locate(out_buf_ch, out_nbufs, gout, &ob, &oc)) {
            snprintf(why, len, "no se encontró el canal %d en el dispositivo agregado", ch);
            return false;
        }
        p->copy[p->ncopy].in_buf = ib;  p->copy[p->ncopy].in_ch = ic;
        p->copy[p->ncopy].out_buf = ob; p->copy[p->ncopy].out_ch = oc;
        p->ncopy++;
    }
    if (p->ncopy == 0) { snprintf(why, len, "ningún canal que copiar"); return false; }
    return true;
}

void avm_bridge_render(avm_bridge_plan* p, uint32_t frames, const float* const* in, float* const* out)
{
    // Todas las salidas del agregado a cero: solo suenan los canales copiados.
    for (int b = 0; b < p->out_nbufs; b++)
        if (out[b]) memset(out[b], 0, sizeof(float) * (size_t)frames * p->out_buf_ch[b]);
    const uint32_t fade0 = p->fade_pos;
    for (int k = 0; k < p->ncopy; k++) {
        const int ib = p->copy[k].in_buf, ob = p->copy[k].out_buf;
        const float* src = in[ib];
        float* dst = out[ob];
        if (dst == NULL || src == NULL) continue;      // sin entrada: queda en silencio
        const uint32_t is = p->in_buf_ch[ib], os = p->out_buf_ch[ob];
        const int ic = p->copy[k].in_ch, oc = p->copy[k].out_ch;
        uint32_t f = 0;
        for (uint32_t pos = fade0; f < frames && pos < AVM_BRIDGE_FADE_FRAMES; f++, pos++)
            dst[(size_t)f * os + (size_t)oc] = src[(size_t)f * is + (size_t)ic] * ((float)(pos + 1) / (float)AVM_BRIDGE_FADE_FRAMES);
        for (; f < frames; f++)
            dst[(size_t)f * os + (size_t)oc] = src[(size_t)f * is + (size_t)ic];   // copia exacta
    }
    uint32_t np = fade0 + frames;
    p->fade_pos = np > AVM_BRIDGE_FADE_FRAMES ? AVM_BRIDGE_FADE_FRAMES : np;
}

static void silence_all(avm_abl* out)
{
    // Cada buffer dentro de SU tamaño: nunca se supone un tamaño común.
    for (uint32_t i = 0; i < out->mNumberBuffers; i++)
        if (out->mBuffers[i].mData && out->mBuffers[i].mDataByteSize)
            memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
}

uint32_t avm_bridge_process(avm_bridge_plan* p, const avm_abl* in, avm_abl* out)
{
    if (out == NULL) return 0;
    const uint32_t nin = in ? in->mNumberBuffers : 0;
    bool shape_ok = (int)out->mNumberBuffers == p->out_nbufs && (int)nin == p->in_nbufs
                 && out->mNumberBuffers <= AVM_BRIDGE_MAX_BUFS && nin <= AVM_BRIDGE_MAX_BUFS;
    for (uint32_t i = 0; shape_ok && i < out->mNumberBuffers; i++)
        if (out->mBuffers[i].mNumberChannels != p->out_buf_ch[i]) shape_ok = false;
    for (uint32_t i = 0; shape_ok && i < nin; i++)
        if (in->mBuffers[i].mNumberChannels != p->in_buf_ch[i]) shape_ok = false;
    if (!shape_ok) {
        atomic_store_explicit(&p->shape_mismatch, 1, memory_order_relaxed);
        silence_all(out);
        return 0;
    }

    // Fotogramas: el mínimo que cabe en TODOS los buffers con datos.
    uint64_t frames = UINT32_MAX;
    bool any = false;
    for (uint32_t i = 0; i < out->mNumberBuffers; i++) {
        const avm_abl_buffer* b = &out->mBuffers[i];
        if (b->mData == NULL || b->mNumberChannels == 0) continue;
        uint64_t f = b->mDataByteSize / (sizeof(float) * (uint64_t)b->mNumberChannels);
        if (f < frames) frames = f;
        any = true;
    }
    for (uint32_t i = 0; i < nin; i++) {
        const avm_abl_buffer* b = &in->mBuffers[i];
        if (b->mData == NULL || b->mNumberChannels == 0) continue;
        uint64_t f = b->mDataByteSize / (sizeof(float) * (uint64_t)b->mNumberChannels);
        if (f < frames) frames = f;
    }
    if (!any) frames = 0;

    const float* ip[AVM_BRIDGE_MAX_BUFS];
    float* op[AVM_BRIDGE_MAX_BUFS];
    for (uint32_t i = 0; i < nin; i++)
        ip[i] = (in->mBuffers[i].mDataByteSize && in->mBuffers[i].mNumberChannels) ? (const float*)in->mBuffers[i].mData : NULL;
    for (uint32_t i = 0; i < out->mNumberBuffers; i++)
        op[i] = (out->mBuffers[i].mDataByteSize && out->mBuffers[i].mNumberChannels) ? (float*)out->mBuffers[i].mData : NULL;
    if (frames > 0) avm_bridge_render(p, (uint32_t)frames, ip, op);

    // Lo que no se ha escrito de cada salida, a cero (dentro de su tamaño).
    for (uint32_t i = 0; i < out->mNumberBuffers; i++) {
        avm_abl_buffer* b = &out->mBuffers[i];
        if (b->mData == NULL) continue;
        size_t used = (size_t)frames * b->mNumberChannels * sizeof(float);
        if (used < b->mDataByteSize) memset((uint8_t*)b->mData + used, 0, b->mDataByteSize - used);
    }
    return (uint32_t)frames;
}

bool avm_bridge_needs_rebuild(const avm_bridge_plan* p,
                              const uint32_t* in_buf_ch, int in_nbufs, const uint32_t* out_buf_ch, int out_nbufs,
                              uint32_t ext_in, uint32_t ext_out)
{
    if (atomic_load_explicit(&p->shape_mismatch, memory_order_relaxed)) return true;
    if (ext_in != p->ext_in || ext_out != p->ext_out) return true;
    if (in_nbufs != p->in_nbufs || out_nbufs != p->out_nbufs) return true;
    for (int i = 0; i < in_nbufs; i++) if (in_buf_ch[i] != p->in_buf_ch[i]) return true;
    for (int i = 0; i < out_nbufs; i++) if (out_buf_ch[i] != p->out_buf_ch[i]) return true;
    return false;
}
