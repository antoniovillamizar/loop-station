// AV Matrix — puentes Dante y EVO con CoreAudio. Ver avm_bridge.h.

#include "avm_bridge.h"
#include "avm_client.h"

#include <Block.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct avm_bridge {
    avm_bridge_kind   kind;
    avm_bridge_config cfg;
    char              other_uid[256];
    bool              suspended;
    avm_bridge_state  state;
    char              msg[512];
    // En marcha
    AudioObjectID     agg;
    AudioDeviceIOProcID proc;
    bool              running;
    avm_bridge_plan   plan;          // lo usa el IOProc mientras funciona
    // Avisos de CoreAudio
    avm_bridge_notify notify;
    void*             ctx;
    AudioObjectPropertyListenerBlock listener;
    AudioObjectID     watched_ext;
    AudioObjectID     watched_avm;
    bool              in_refresh;
    bool              destroyed;      // tras avm_bridge_destroy: los avisos pendientes no hacen nada
};

static const AudioObjectPropertyAddress kDevicesAddr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kRateAddr    = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kAliveAddr   = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

static avm_device_kind avm_kind(const avm_bridge* b) { return b->kind == AVM_BRIDGE_DANTE ? AVM_DEV_DANTE : AVM_DEV_EVO; }

// --- IOProc (hilo de IO del agregado) --------------------------------------------

static OSStatus bridge_io(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* in, const AudioTimeStamp* inTime,
                          AudioBufferList* out, const AudioTimeStamp* outTime, void* ctx)
{
    (void)dev; (void)now; (void)inTime; (void)outTime;
    avm_bridge* b = (avm_bridge*)ctx;
    avm_bridge_plan* p = &b->plan;
    if (out == NULL) return noErr;
    // Si la forma de los buffers no es la del plan (cambio de configuración a
    // mitad), se entrega silencio en vez de copiar canales equivocados.
    bool shape_ok = (int)out->mNumberBuffers == p->out_nbufs && in != NULL && (int)in->mNumberBuffers == p->in_nbufs;
    UInt32 frames = 0;
    for (UInt32 i = 0; shape_ok && i < out->mNumberBuffers; i++) {
        if (out->mBuffers[i].mNumberChannels != p->out_buf_ch[i]) shape_ok = false;
        else if (frames == 0 && out->mBuffers[i].mNumberChannels)
            frames = out->mBuffers[i].mDataByteSize / (UInt32)(sizeof(float) * out->mBuffers[i].mNumberChannels);
    }
    for (UInt32 i = 0; shape_ok && i < in->mNumberBuffers; i++)
        if (in->mBuffers[i].mNumberChannels != p->in_buf_ch[i]
            || (in->mBuffers[i].mData && in->mBuffers[i].mNumberChannels
                && in->mBuffers[i].mDataByteSize / (UInt32)(sizeof(float) * in->mBuffers[i].mNumberChannels) < frames))
            shape_ok = false;
    if (!shape_ok) {
        for (UInt32 i = 0; i < out->mNumberBuffers; i++)
            if (out->mBuffers[i].mData) memset(out->mBuffers[i].mData, 0, out->mBuffers[i].mDataByteSize);
        return noErr;
    }
    const float* ip[AVM_BRIDGE_MAX_BUFS];
    float* op[AVM_BRIDGE_MAX_BUFS];
    for (int i = 0; i < p->in_nbufs; i++) ip[i] = (const float*)in->mBuffers[i].mData;
    for (int i = 0; i < p->out_nbufs; i++) op[i] = (float*)out->mBuffers[i].mData;
    avm_bridge_render(p, frames, ip, op);
    return noErr;
}

// --- Forma del agregado ---------------------------------------------------------

static int buffer_shape(AudioObjectID dev, AudioObjectPropertyScope scope, uint32_t* ch, int max)
{
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreamConfiguration, scope, kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, NULL, &size) != noErr || size == 0) return 0;
    AudioBufferList* l = (AudioBufferList*)malloc(size);
    if (l == NULL) return -1;
    int n = -1;
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, l) == noErr && (int)l->mNumberBuffers <= max) {
        n = (int)l->mNumberBuffers;
        for (int i = 0; i < n; i++) ch[i] = l->mBuffers[i].mNumberChannels;
    }
    free(l);
    return n;
}

// Un agregado del usuario que contiene un dispositivo de AV Matrix también se
// considera de AV Matrix: usarlo crearía realimentación.
static bool contains_avmatrix(AudioObjectID dev)
{
    AudioObjectPropertyAddress a = { kAudioAggregateDevicePropertyFullSubDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFArrayRef list = NULL;
    UInt32 size = sizeof(list);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &list) != noErr || list == NULL) return false;
    bool found = false;
    for (CFIndex i = 0; i < CFArrayGetCount(list) && !found; i++) {
        CFTypeRef v = CFArrayGetValueAtIndex(list, i);
        char uid[256];
        if (v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString((CFStringRef)v, uid, sizeof(uid), kCFStringEncodingUTF8))
            found = strncmp(uid, AVM_UID_PREFIX, strlen(AVM_UID_PREFIX)) == 0;
    }
    CFRelease(list);
    return found;
}

// --- Arranque y parada ------------------------------------------------------------

static void bridge_stop(avm_bridge* b)
{
    if (b->agg != kAudioObjectUnknown) {
        if (b->proc) {
            AudioDeviceStop(b->agg, b->proc);
            AudioDeviceDestroyIOProcID(b->agg, b->proc);
        }
        AudioHardwareDestroyAggregateDevice(b->agg);
    }
    b->agg = kAudioObjectUnknown;
    b->proc = NULL;
    b->running = false;
}

static void dict_set_str(CFMutableDictionaryRef d, const char* key, const char* value)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFStringRef v = CFStringCreateWithCString(NULL, value, kCFStringEncodingUTF8);
    if (k && v) CFDictionarySetValue(d, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

static void dict_set_int(CFMutableDictionaryRef d, const char* key, int value)
{
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFNumberRef v = CFNumberCreate(NULL, kCFNumberIntType, &value);
    if (k && v) CFDictionarySetValue(d, k, v);
    if (k) CFRelease(k);
    if (v) CFRelease(v);
}

static bool bridge_start(avm_bridge* b, const avm_hw_device* avm, const avm_hw_device* ext)
{
    const bool dante = b->kind == AVM_BRIDGE_DANTE;
    char uid[128];
    snprintf(uid, sizeof(uid), "AVMatrix_Puente_%s_%d", dante ? "Dante" : "EVO", (int)getpid());

    CFMutableDictionaryRef desc = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableArrayRef subs = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef sa = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFMutableDictionaryRef se = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    if (!desc || !subs || !sa || !se) {
        if (desc) CFRelease(desc);
        if (subs) CFRelease(subs);
        if (sa) CFRelease(sa);
        if (se) CFRelease(se);
        snprintf(b->msg, sizeof(b->msg), "Sin memoria para crear el puente.");
        return false;
    }
    dict_set_str(desc, kAudioAggregateDeviceUIDKey, uid);
    dict_set_str(desc, kAudioAggregateDeviceNameKey, dante ? "AV Matrix → Dante (privado)" : "EVO → AV Matrix (privado)");
    dict_set_int(desc, kAudioAggregateDeviceIsPrivateKey, 1);
    dict_set_int(desc, kAudioAggregateDeviceIsStackedKey, 0);
    // Primero AV Matrix (con compensación de deriva), después el externo (reloj).
    dict_set_str(sa, kAudioSubDeviceUIDKey, avm->uid);
    dict_set_int(sa, kAudioSubDeviceDriftCompensationKey, 1);
    dict_set_int(sa, kAudioSubDeviceDriftCompensationQualityKey, 0x7F);
    dict_set_str(se, kAudioSubDeviceUIDKey, ext->uid);
    dict_set_int(se, kAudioSubDeviceDriftCompensationKey, 0);
    CFArrayAppendValue(subs, sa);
    CFArrayAppendValue(subs, se);
    CFStringRef subsKey = CFStringCreateWithCString(NULL, kAudioAggregateDeviceSubDeviceListKey, kCFStringEncodingUTF8);
    CFDictionarySetValue(desc, subsKey, subs);
    CFRelease(subsKey);
    dict_set_str(desc, kAudioAggregateDeviceMainSubDeviceKey, ext->uid);
    dict_set_str(desc, kAudioAggregateDeviceClockDeviceKey, ext->uid);

    AudioObjectID agg = kAudioObjectUnknown;
    OSStatus err = AudioHardwareCreateAggregateDevice(desc, &agg);
    CFRelease(sa); CFRelease(se); CFRelease(subs); CFRelease(desc);
    if (err != noErr || agg == kAudioObjectUnknown) {
        snprintf(b->msg, sizeof(b->msg), "CoreAudio no pudo crear el puente (error %d).", (int)err);
        return false;
    }
    b->agg = agg;

    // Misma frecuencia que AV Matrix (ya comprobado que el externo coincide) y
    // un buffer corto: menos retardo y menos audio en tránsito.
    Float64 rate = avm->rate;
    AudioObjectSetPropertyData(agg, &kRateAddr, 0, NULL, sizeof(rate), &rate);
    UInt32 frames = 256;
    AudioObjectPropertyAddress bs = { kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectSetPropertyData(agg, &bs, 0, NULL, sizeof(frames), &frames);

    uint32_t in_ch[AVM_BRIDGE_MAX_BUFS], out_ch[AVM_BRIDGE_MAX_BUFS];
    int nin = buffer_shape(agg, kAudioObjectPropertyScopeInput, in_ch, AVM_BRIDGE_MAX_BUFS);
    int nout = buffer_shape(agg, kAudioObjectPropertyScopeOutput, out_ch, AVM_BRIDGE_MAX_BUFS);
    char why[256];
    if (nin < 0 || nout < 0
        || !avm_bridge_make_plan(&b->cfg, in_ch, nin, out_ch, nout,
                                 dante ? (uint32_t)AVM_DANTE_CHANNELS : 0, dante ? 0 : (uint32_t)AVM_EVO_CHANNELS,
                                 ext->in_channels, ext->out_channels, &b->plan, why, sizeof(why))) {
        snprintf(b->msg, sizeof(b->msg), "No se pudo preparar el puente: %s.", nin < 0 || nout < 0 ? "forma de los buffers" : why);
        bridge_stop(b);
        return false;
    }
    err = AudioDeviceCreateIOProcID(agg, bridge_io, b, &b->proc);
    if (err == noErr) err = AudioDeviceStart(agg, b->proc);
    if (err != noErr) {
        snprintf(b->msg, sizeof(b->msg), "CoreAudio no pudo arrancar el puente (error %d).", (int)err);
        bridge_stop(b);
        return false;
    }
    b->running = true;
    return true;
}

// --- Avisos del sistema -----------------------------------------------------------

static void watch(avm_bridge* b, AudioObjectID ext, AudioObjectID avm)
{
    if (ext != b->watched_ext) {
        if (b->watched_ext != kAudioObjectUnknown) {
            AudioObjectRemovePropertyListenerBlock(b->watched_ext, &kRateAddr, dispatch_get_main_queue(), b->listener);
            AudioObjectRemovePropertyListenerBlock(b->watched_ext, &kAliveAddr, dispatch_get_main_queue(), b->listener);
        }
        if (ext != kAudioObjectUnknown) {
            AudioObjectAddPropertyListenerBlock(ext, &kRateAddr, dispatch_get_main_queue(), b->listener);
            AudioObjectAddPropertyListenerBlock(ext, &kAliveAddr, dispatch_get_main_queue(), b->listener);
        }
        b->watched_ext = ext;
    }
    if (avm != b->watched_avm) {
        if (b->watched_avm != kAudioObjectUnknown)
            AudioObjectRemovePropertyListenerBlock(b->watched_avm, &kRateAddr, dispatch_get_main_queue(), b->listener);
        if (avm != kAudioObjectUnknown)
            AudioObjectAddPropertyListenerBlock(avm, &kRateAddr, dispatch_get_main_queue(), b->listener);
        b->watched_avm = avm;
    }
}

avm_bridge* avm_bridge_create(avm_bridge_kind kind, avm_bridge_notify notify, void* ctx)
{
    avm_bridge* b = (avm_bridge*)calloc(1, sizeof(avm_bridge));
    if (b == NULL) return NULL;
    b->kind = kind;
    avm_bridge_config_default(&b->cfg, kind);
    b->notify = notify;
    b->ctx = ctx;
    b->agg = kAudioObjectUnknown;
    b->watched_ext = kAudioObjectUnknown;
    b->watched_avm = kAudioObjectUnknown;
    b->state = AVM_BRIDGE_OFF;
    b->listener = Block_copy(^(UInt32 n, const AudioObjectPropertyAddress* a) {
        (void)n; (void)a;
        avm_bridge_refresh(b);
    });
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &kDevicesAddr, dispatch_get_main_queue(), b->listener);
    return b;
}

// Detiene el puente y quita los avisos. La estructura no se libera: un aviso
// que ya estaba en la cola principal podría llegar después y la usaría (solo se
// llama al salir de la app, así que la memoria se recupera al terminar).
void avm_bridge_destroy(avm_bridge* b)
{
    if (b == NULL || b->destroyed) return;
    b->destroyed = true;
    bridge_stop(b);
    watch(b, kAudioObjectUnknown, kAudioObjectUnknown);
    AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &kDevicesAddr, dispatch_get_main_queue(), b->listener);
    b->notify = NULL;
}

void avm_bridge_refresh(avm_bridge* b)
{
    if (b == NULL || b->destroyed) return;
    if (b->in_refresh) return;   // crear o destruir el agregado vuelve a avisar
    b->in_refresh = true;

    avm_hw_device avm, ext;
    bool avm_ok = false, ext_ok = false;
    AudioObjectID avm_id = avm_find_device(avm_kind(b));
    if (avm_id != kAudioObjectUnknown) avm_ok = avm_hw_device_info(avm_id, &avm) == noErr && avm.alive;
    ext_ok = avm_hw_device_by_uid(b->cfg.device_uid, &ext) == noErr && ext.alive;

    avm_bridge_env env;
    memset(&env, 0, sizeof(env));
    env.avm_present = avm_ok;
    env.avm_rate = avm_ok ? avm.rate : 0;
    env.ext_present = ext_ok;
    env.ext_is_avmatrix = ext_ok && (ext.is_avmatrix || contains_avmatrix(ext.id));
    env.ext_used_by_other = b->other_uid[0] && strcmp(b->other_uid, b->cfg.device_uid) == 0;
    env.ext_channels = ext_ok ? (b->kind == AVM_BRIDGE_DANTE ? ext.out_channels : ext.in_channels) : 0;
    env.ext_rate = ext_ok ? ext.rate : 0;
    if (ext_ok) snprintf(env.ext_name, sizeof(env.ext_name), "%s", ext.name);
    env.suspended = b->suspended;

    char msg[512];
    avm_bridge_state want = avm_bridge_evaluate(&b->cfg, &env, msg, sizeof(msg));
    if (want != AVM_BRIDGE_RUN) {
        if (b->running) bridge_stop(b);              // detener de forma segura
        b->state = want;
        snprintf(b->msg, sizeof(b->msg), "%s", msg);
    } else if (b->running) {
        b->state = AVM_BRIDGE_RUN;
        snprintf(b->msg, sizeof(b->msg), "%s", msg);
    } else if (bridge_start(b, &avm, &ext)) {
        b->state = AVM_BRIDGE_RUN;
        snprintf(b->msg, sizeof(b->msg), "%s", msg);
    } else {
        b->state = AVM_BRIDGE_ERROR;                 // b->msg ya explica el motivo
    }
    watch(b, ext_ok ? ext.id : kAudioObjectUnknown, avm_ok ? avm.id : kAudioObjectUnknown);
    b->in_refresh = false;
    if (b->notify) b->notify(b->ctx);
}

void avm_bridge_configure(avm_bridge* b, const avm_bridge_config* c)
{
    bool same_route = strcmp(c->device_uid, b->cfg.device_uid) == 0 && memcmp(c->channel, b->cfg.channel, sizeof(c->channel)) == 0;
    if (b->running && (!same_route || !c->enabled)) bridge_stop(b);   // un plan nuevo exige un agregado nuevo
    b->cfg = *c;
    b->cfg.kind = b->kind;
    avm_bridge_refresh(b);
}

void avm_bridge_set_other_uid(avm_bridge* b, const char* uid)
{
    snprintf(b->other_uid, sizeof(b->other_uid), "%s", uid ? uid : "");
}

void avm_bridge_set_suspended(avm_bridge* b, bool suspended)
{
    b->suspended = suspended;
    avm_bridge_refresh(b);
}

avm_bridge_state avm_bridge_get_state(const avm_bridge* b, char* msg, size_t len)
{
    if (msg && len) snprintf(msg, len, "%s", b->msg);
    return b->state;
}

bool avm_bridge_is_running(const avm_bridge* b) { return b->running; }
const avm_bridge_config* avm_bridge_get_config(const avm_bridge* b) { return &b->cfg; }

OSStatus avm_bridge_match_rate(avm_bridge* b)
{
    avm_hw_device ext, avm;
    AudioObjectID avm_id = avm_find_device(avm_kind(b));
    if (avm_id == kAudioObjectUnknown || avm_hw_device_info(avm_id, &avm) != noErr) return kAudioHardwareBadDeviceError;
    if (avm_hw_device_by_uid(b->cfg.device_uid, &ext) != noErr || ext.is_avmatrix) return kAudioHardwareBadDeviceError;
    OSStatus err = avm_hw_set_rate(ext.id, avm.rate);
    avm_bridge_refresh(b);
    return err;
}
