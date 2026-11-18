// AV Matrix — puentes Dante y EVO con CoreAudio. Ver avm_bridge.h.

#include "avm_bridge.h"
#include "avm_client.h"

#include <Block.h>
#include <dispatch/dispatch.h>
#include <stddef.h>
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
    AudioObjectID     ext_id;         // dispositivo externo con el que se preparó el plan
    bool              in_refresh;
    bool              destroyed;      // tras avm_bridge_destroy: los avisos pendientes no hacen nada
};

static const AudioObjectPropertyAddress kDevicesAddr = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kRateAddr    = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kAliveAddr   = { kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
// Cambios de streams o de canales (p. ej. la EVO pasa de 8 a 16 canales).
static const AudioObjectPropertyAddress kCfgInAddr   = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kCfgOutAddr  = { kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kStreamsAddr = { kAudioDevicePropertyStreams, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

static avm_device_kind avm_kind(const avm_bridge* b) { return b->kind == AVM_BRIDGE_DANTE ? AVM_DEV_DANTE : AVM_DEV_EVO; }

// --- IOProc (hilo de IO del agregado) --------------------------------------------

// avm_abl (avm_bridge_core.h) debe tener exactamente la disposición en memoria
// de AudioBufferList: así el callback real se prueba también fuera de macOS.
_Static_assert(sizeof(avm_abl_buffer) == sizeof(AudioBuffer), "AudioBuffer");
_Static_assert(offsetof(avm_abl_buffer, mNumberChannels) == offsetof(AudioBuffer, mNumberChannels), "mNumberChannels");
_Static_assert(offsetof(avm_abl_buffer, mDataByteSize) == offsetof(AudioBuffer, mDataByteSize), "mDataByteSize");
_Static_assert(offsetof(avm_abl_buffer, mData) == offsetof(AudioBuffer, mData), "mData");
_Static_assert(offsetof(avm_abl, mBuffers) == offsetof(AudioBufferList, mBuffers), "mBuffers");

// El callback solo traduce tipos: avm_bridge_process valida CADA buffer (número,
// canales y tamaño) y, si algo no cuadra, entrega silencio dentro de los límites
// de cada uno y avisa al hilo principal para reconstruir el puente.
static OSStatus bridge_io(AudioObjectID dev, const AudioTimeStamp* now, const AudioBufferList* in, const AudioTimeStamp* inTime,
                          AudioBufferList* out, const AudioTimeStamp* outTime, void* ctx)
{
    (void)dev; (void)now; (void)inTime; (void)outTime;
    avm_bridge* b = (avm_bridge*)ctx;
    avm_bridge_process(&b->plan, (const avm_abl*)in, (avm_abl*)out);
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
    b->ext_id = ext->id;
    return true;
}

// --- Avisos del sistema -----------------------------------------------------------

static void watch(avm_bridge* b, AudioObjectID ext, AudioObjectID avm)
{
    if (ext != b->watched_ext) {
        const AudioObjectPropertyAddress* addrs[5] = { &kRateAddr, &kAliveAddr, &kCfgInAddr, &kCfgOutAddr, &kStreamsAddr };
        if (b->watched_ext != kAudioObjectUnknown)
            for (int i = 0; i < 5; i++) AudioObjectRemovePropertyListenerBlock(b->watched_ext, addrs[i], dispatch_get_main_queue(), b->listener);
        if (ext != kAudioObjectUnknown)
            for (int i = 0; i < 5; i++) AudioObjectAddPropertyListenerBlock(ext, addrs[i], dispatch_get_main_queue(), b->listener);
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

// ¿Cambió algo que invalida el plan en marcha? Dispositivo externo distinto
// (p. ej. reconectado con otro id), sus canales, la forma de los buffers del
// agregado, o el hilo de IO avisó de buffers con otra forma.
static bool bridge_changed(avm_bridge* b, const avm_hw_device* ext)
{
    if (ext->id != b->ext_id) return true;
    uint32_t in_ch[AVM_BRIDGE_MAX_BUFS], out_ch[AVM_BRIDGE_MAX_BUFS];
    int nin = buffer_shape(b->agg, kAudioObjectPropertyScopeInput, in_ch, AVM_BRIDGE_MAX_BUFS);
    int nout = buffer_shape(b->agg, kAudioObjectPropertyScopeOutput, out_ch, AVM_BRIDGE_MAX_BUFS);
    if (nin < 0 || nout < 0) return true;
    return avm_bridge_needs_rebuild(&b->plan, in_ch, nin, out_ch, nout, ext->in_channels, ext->out_channels);
}

void avm_bridge_poll(avm_bridge* b)
{
    if (b && !b->destroyed && b->running && atomic_load_explicit(&b->plan.shape_mismatch, memory_order_relaxed))
        avm_bridge_refresh(b);
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
    // Confirmado en el Mac: la EVO8 expone 6 entradas; 1-4 físicas, 5-6 loopback.
    if (ext_ok && b->kind == AVM_BRIDGE_EVO && strstr(ext.name, "EVO8")) env.loopback_from = 5;

    char msg[512];
    avm_bridge_state want = avm_bridge_evaluate(&b->cfg, &env, msg, sizeof(msg));
    if (want != AVM_BRIDGE_RUN) {
        if (b->running) bridge_stop(b);              // detener de forma segura
        b->state = want;
        snprintf(b->msg, sizeof(b->msg), "%s", msg);
    } else if (b->running && !bridge_changed(b, &ext)) {
        b->state = AVM_BRIDGE_RUN;
        snprintf(b->msg, sizeof(b->msg), "%s", msg);
    } else {
        // Arranque, o reconstrucción tras un cambio de streams/canales o de
        // dispositivo: primero se detiene el IOProc y se destruye el agregado
        // viejo; después, agregado y plan nuevos. Siempre aquí (hilo principal),
        // nunca en el hilo de audio.
        bool rebuild = b->running;
        if (rebuild) bridge_stop(b);
        if (bridge_start(b, &avm, &ext)) {
            b->state = AVM_BRIDGE_RUN;
            snprintf(b->msg, sizeof(b->msg), "%s%s", msg, rebuild ? " (Puente reconstruido: cambió la configuración del dispositivo.)" : "");
        } else {
            b->state = AVM_BRIDGE_ERROR;             // b->msg ya explica el motivo
        }
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
