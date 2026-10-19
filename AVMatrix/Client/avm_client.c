// AV Matrix — funciones de cliente CoreAudio. Ver avm_client.h.

#include "avm_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

const char* avm_device_uid(avm_device_kind kind)
{
    switch (kind) {
        case AVM_DEV_SMAART:   return AVM_UID_SMAART;
        case AVM_DEV_LISTENTO: return AVM_UID_LISTENTO;
        case AVM_DEV_MUSIC:    return AVM_UID_MUSIC;
        default:               return "";
    }
}

const char* avm_device_label(avm_device_kind kind)
{
    switch (kind) {
        case AVM_DEV_SMAART:   return "AV Matrix Smaart";
        case AVM_DEV_LISTENTO: return "AV Matrix LISTENTO";
        case AVM_DEV_MUSIC:    return "AV Matrix Música";
        default:               return "";
    }
}

AudioObjectID avm_find_device(avm_device_kind kind)
{
    CFStringRef uid = CFStringCreateWithCString(NULL, avm_device_uid(kind), kCFStringEncodingUTF8);
    if (uid == NULL) return kAudioObjectUnknown;
    AudioObjectPropertyAddress a = { kAudioHardwarePropertyTranslateUIDToDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectID dev = kAudioObjectUnknown;
    UInt32 size = sizeof(dev);
    OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, sizeof(uid), &uid, &size, &dev);
    CFRelease(uid);
    return err == noErr ? dev : kAudioObjectUnknown;
}

OSStatus avm_get_matrix(avm_matrix* out)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    AudioObjectPropertyAddress a = { AVM_PROPERTY_MATRIX, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof(plist);
    OSStatus err = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &plist);
    if (err != noErr) return err;
    if (plist == NULL) return kAudioHardwareUnspecifiedError;
    if (CFGetTypeID(plist) != CFDataGetTypeID() || CFDataGetLength((CFDataRef)plist) != (CFIndex)sizeof(out->cell)) {
        CFRelease(plist);
        return kAudioHardwareBadPropertySizeError;
    }
    memcpy(out->cell, CFDataGetBytePtr((CFDataRef)plist), sizeof(out->cell));
    CFRelease(plist);
    return noErr;
}

OSStatus avm_set_matrix(const avm_matrix* m)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    CFDataRef data = CFDataCreate(NULL, (const UInt8*)m->cell, sizeof(m->cell));
    if (data == NULL) return kAudioHardwareUnspecifiedError;
    AudioObjectPropertyAddress a = { AVM_PROPERTY_MATRIX, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = data;
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(plist), &plist);
    CFRelease(data);
    return err;
}

OSStatus avm_get_rate_health(int* out)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    AudioObjectPropertyAddress a = { AVM_PROPERTY_RATE_STATE, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = NULL;
    UInt32 size = sizeof(plist);
    OSStatus err = AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, &plist);
    if (err != noErr) return err;
    if (plist == NULL) return kAudioHardwareUnspecifiedError;
    SInt32 v = -1;
    bool ok = CFGetTypeID(plist) == CFNumberGetTypeID() && CFNumberGetValue((CFNumberRef)plist, kCFNumberSInt32Type, &v);
    CFRelease(plist);
    if (!ok) return kAudioHardwareBadPropertySizeError;
    *out = (int)v;
    return noErr;
}

OSStatus avm_recover_rate(Float64 rate)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    CFNumberRef n = CFNumberCreate(NULL, kCFNumberFloat64Type, &rate);
    if (n == NULL) return kAudioHardwareUnspecifiedError;
    AudioObjectPropertyAddress a = { AVM_PROPERTY_RATE_STATE, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    CFPropertyListRef plist = n;
    OSStatus err = AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(plist), &plist);
    CFRelease(n);
    return err;
}

OSStatus avm_get_sample_rate(Float64* out)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(*out);
    return AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, out);
}

OSStatus avm_set_sample_rate(Float64 rate)
{
    AudioObjectID dev = avm_find_device(AVM_DEV_SMAART);
    if (dev == kAudioObjectUnknown) return kAudioHardwareBadDeviceError;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectSetPropertyData(dev, &a, 0, NULL, sizeof(rate), &rate);
}

static AudioObjectID avm_find_stream(avm_device_kind kind, bool input)
{
    AudioObjectID dev = avm_find_device(kind);
    if (dev == kAudioObjectUnknown) return kAudioObjectUnknown;
    AudioObjectPropertyAddress a = { kAudioDevicePropertyStreams,
        input ? kAudioObjectPropertyScopeInput : kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
    AudioObjectID streams[4];
    UInt32 size = sizeof(streams);
    if (AudioObjectGetPropertyData(dev, &a, 0, NULL, &size, streams) != noErr || size < sizeof(AudioObjectID))
        return kAudioObjectUnknown;
    return streams[0];
}

OSStatus avm_get_physical_format(avm_device_kind kind, bool input, AudioStreamBasicDescription* out)
{
    AudioObjectID s = avm_find_stream(kind, input);
    if (s == kAudioObjectUnknown) return kAudioHardwareBadStreamError;
    AudioObjectPropertyAddress a = { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(*out);
    return AudioObjectGetPropertyData(s, &a, 0, NULL, &size, out);
}

OSStatus avm_set_physical_format(avm_device_kind kind, bool input, avm_format fmt)
{
    AudioStreamBasicDescription d;
    OSStatus err = avm_get_physical_format(kind, input, &d);
    if (err != noErr) return err;
    UInt32 ch = d.mChannelsPerFrame;
    UInt32 bps = avm_format_bytes_per_sample(fmt);
    d.mFormatID = kAudioFormatLinearPCM;
    d.mFormatFlags = (fmt == AVM_FMT_FLOAT32 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger)
                     | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d.mBitsPerChannel = avm_format_bits(fmt);
    d.mBytesPerFrame = bps * ch;
    d.mBytesPerPacket = bps * ch;
    d.mFramesPerPacket = 1;
    AudioObjectID s = avm_find_stream(kind, input);
    AudioObjectPropertyAddress a = { kAudioStreamPropertyPhysicalFormat, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    return AudioObjectSetPropertyData(s, &a, 0, NULL, sizeof(d), &d);
}

const char* avm_format_name(avm_format fmt)
{
    switch (fmt) {
        case AVM_FMT_FLOAT32: return "32 bits coma flotante";
        case AVM_FMT_SINT32:  return "32 bits entero";
        case AVM_FMT_SINT24:  return "24 bits entero";
        case AVM_FMT_SINT16:  return "16 bits entero";
        default:              return "?";
    }
}

int avm_format_from_asbd(const AudioStreamBasicDescription* d)
{
    if (d->mFormatID != kAudioFormatLinearPCM) return -1;
    if (d->mFormatFlags & kAudioFormatFlagIsFloat) return d->mBitsPerChannel == 32 ? AVM_FMT_FLOAT32 : -1;
    switch (d->mBitsPerChannel) {
        case 32: return AVM_FMT_SINT32;
        case 24: return AVM_FMT_SINT24;
        case 16: return AVM_FMT_SINT16;
    }
    return -1;
}

void avm_source_name(int src, char* buf, size_t len)
{
    if (src < AVM_SRC_LINK_BASE)       snprintf(buf, len, "Smaart Salida %d", src - AVM_SRC_SMAART_BASE + 1);
    else if (src < AVM_SRC_MUSIC_BASE) snprintf(buf, len, "LISTENTO Retorno %d", src - AVM_SRC_LINK_BASE + 1);
    else                               snprintf(buf, len, "Música %s", src == AVM_SRC_MUSIC_BASE ? "L" : "R");
}

void avm_dest_name(int dst, char* buf, size_t len)
{
    if (dst == AVM_DST_LOCAL_REFERENCE) snprintf(buf, len, "Smaart Entrada 8 (REFERENCIA LOCAL)");
    else if (dst < AVM_DST_LINK_BASE)   snprintf(buf, len, "Smaart Entrada %d", dst - AVM_DST_SMAART_BASE + 1);
    else                                snprintf(buf, len, "LISTENTO Envío %d", dst - AVM_DST_LINK_BASE + 1);
}

void avm_source_token(int src, char* buf, size_t len)
{
    if (src < AVM_SRC_LINK_BASE)       snprintf(buf, len, "smaart.out.%d", src - AVM_SRC_SMAART_BASE + 1);
    else if (src < AVM_SRC_MUSIC_BASE) snprintf(buf, len, "listento.out.%d", src - AVM_SRC_LINK_BASE + 1);
    else                               snprintf(buf, len, "music.%s", src == AVM_SRC_MUSIC_BASE ? "L" : "R");
}

void avm_dest_token(int dst, char* buf, size_t len)
{
    if (dst < AVM_DST_LINK_BASE) snprintf(buf, len, "smaart.in.%d", dst - AVM_DST_SMAART_BASE + 1);
    else                         snprintf(buf, len, "listento.in.%d", dst - AVM_DST_LINK_BASE + 1);
}

int avm_parse_source(const char* t)
{
    char buf[32];
    for (int s = 0; s < AVM_NUM_SOURCES; s++) {
        avm_source_token(s, buf, sizeof(buf));
        if (strcasecmp(t, buf) == 0) return s;
    }
    return -1;
}

int avm_parse_dest(const char* t)
{
    char buf[32];
    for (int d = 0; d < AVM_NUM_DESTS; d++) {
        avm_dest_token(d, buf, sizeof(buf));
        if (strcasecmp(t, buf) == 0) return d;
    }
    return -1;
}
