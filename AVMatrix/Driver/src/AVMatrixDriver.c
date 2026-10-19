// AV Matrix — AudioServerPlugIn (driver HAL de CoreAudio) para macOS.
//
// Publica tres dispositivos virtuales que comparten un reloj y una matriz:
//
//   "AV Matrix Smaart"    8 entradas + 8 salidas   (el que se selecciona en Smaart)
//   "AV Matrix LISTENTO"  8 entradas + 8 salidas   (el que se selecciona en LISTENTO)
//   "AV Matrix Música"    2 salidas                (salida del sistema / Apple Music / DAW)
//
// Las salidas de los tres son fuentes de la matriz; las entradas de Smaart y de
// LISTENTO son destinos. Ver avm_core.h. La matriz se lee y se escribe mediante
// la propiedad personalizada 'mtrx' (CFData de 16 x 18 bytes, fila = destino) y
// el mixer (volumen y mute por canal, solo atenuación) mediante 'mixr'.
//
// El driver solo transporta, enruta y atenúa. No hay ecualización, efectos,
// análisis ni delay ajustable.

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <os/log.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avm_core.h"
#include "avm_persist.h"
#include "avm_rate.h"

// ---------------------------------------------------------------------------
#pragma mark Constantes

#define kAVM_BundleID             "com.antoniovillamizar.avmatrix.driver"
#define kAVM_Manufacturer         "AV Matrix"
#define kAVM_ZeroTimeStampPeriod  16384u
#define kAVM_ClockDomain          0x41564D58u   // 'AVMX': los tres dispositivos comparten reloj

// Propiedad personalizada de la matriz (en el plug-in y en cada dispositivo).
#define kAVM_PropertyMatrix       'mtrx'
// Estado de la frecuencia (CFNumber). Lectura: 0 coherente, 1 cambio en curso,
// 2 inconsistente (el audio no arranca hasta recuperar). Escritura: CFNumber con
// la frecuencia a aplicar en los tres dispositivos (0 = la actual): es la vía de
// recuperación que no depende de que el host reenvíe una frecuencia sin cambios.
#define kAVM_PropertyRateState    'rsta'
// Medidores de nivel (solo lectura, CFData; formato en avm_core.h, avm_meters_pack).
// Leerla no arranca audio ni modifica nada: la app la consulta periódicamente.
#define kAVM_PropertyMeters       'metr'
// Mixer (CFData, formato en avm_core.h, avm_mixer_pack). Escritura: se rechaza
// entera si algún valor no es válido (ganancia fuera de [0, 1], es decir, sin
// amplificación). Se guarda y se recupera al reiniciar, como la matriz.
#define kAVM_PropertyMixer        'mixr'

static const Float64 kAVM_SampleRates[] = { 44100.0, 48000.0, 96000.0 };
#define kAVM_NumSampleRates       3

enum {
    kObj_PlugIn          = kAudioObjectPlugInObject,  // 1
    kObj_Dev_Smaart      = 2,
    kObj_Str_Smaart_In   = 3,
    kObj_Str_Smaart_Out  = 4,
    kObj_Dev_Link        = 5,
    kObj_Str_Link_In     = 6,
    kObj_Str_Link_Out    = 7,
    kObj_Dev_Music       = 8,
    kObj_Str_Music_Out   = 9,
    kObj_Max             = 10
};

#define kAVM_NumDevices 3

typedef struct {
    AudioObjectID   device;
    AudioObjectID   inStream;     // 0 si no tiene entradas
    AudioObjectID   outStream;
    const char*     name;
    const char*     uid;
    const char*     modelUID;
    UInt32          inChannels;
    UInt32          outChannels;
    avm_src_group   srcGroup;     // a qué fuentes escriben sus salidas
    int             dstGroup;     // de qué destinos leen sus entradas (-1: ninguno)
    Boolean         canBeDefaultOutput;
    const char*     inChannelName;   // "%u" se sustituye por el número de canal
    const char*     outChannelName;
} AVM_DeviceDesc;

static const AVM_DeviceDesc kDevices[kAVM_NumDevices] = {
    { kObj_Dev_Smaart, kObj_Str_Smaart_In, kObj_Str_Smaart_Out,
      "AV Matrix Smaart", "AVMatrix_Smaart_UID", "AVMatrix_Model_8x8",
      8, 8, AVM_SRC_GROUP_SMAART, AVM_DST_GROUP_SMAART, false,
      "Entrada %u", "Salida %u" },
    { kObj_Dev_Link, kObj_Str_Link_In, kObj_Str_Link_Out,
      "AV Matrix LISTENTO", "AVMatrix_Listento_UID", "AVMatrix_Model_8x8",
      8, 8, AVM_SRC_GROUP_LINK, AVM_DST_GROUP_LINK, false,
      "Envío %u", "Retorno %u" },
    { kObj_Dev_Music, 0, kObj_Str_Music_Out,
      "AV Matrix Música", "AVMatrix_Music_UID", "AVMatrix_Model_Stereo",
      0, 2, AVM_SRC_GROUP_MUSIC, -1, true,
      NULL, "Música %u" },
};

// Acciones de cambio de configuración: [tipo 8 bits][generación 24 bits][valor 32 bits].
#define kAVM_ActionSetRate    1ull
#define kAVM_ActionSetFormat  2ull
#define AVM_ACTION(kind, gen, value) (((UInt64)(kind) << 56) | (((UInt64)(gen) & 0xFFFFFFull) << 32) | ((UInt64)(value) & 0xFFFFFFFFull))
#define AVM_ACTION_KIND(a)      ((a) >> 56)
#define AVM_ACTION_GEN(a)       ((UInt32)(((a) >> 32) & 0xFFFFFFull))
#define AVM_ACTION_VALUE(a)     ((UInt32)((a) & 0xFFFFFFFFull))

// Una transacción de frecuencia sin Perform ni Abort caduca a los 5 s.
#define kAVM_RateTransactionTimeoutNs 5000000000ull

// ---------------------------------------------------------------------------
#pragma mark Estado

static pthread_mutex_t          gStateMutex = PTHREAD_MUTEX_INITIALIZER;
// Guardado ordenado de la matriz y del mixer: lo guardado es siempre el último
// ajuste aplicado, aunque dos cambios lleguen a la vez (ver avm_persist.h).
static avm_persist              gPersistMatrix;
static avm_persist              gPersistMixer;
static pthread_mutex_t          gClockMutex = PTHREAD_MUTEX_INITIALIZER;
static UInt32                   gRefCount = 0;
static AudioServerPlugInHostRef gHost = NULL;

// Frecuencia, transacción de cambio de frecuencia y contadores de IO.
// Protegido por gClockMutex.
static avm_rate_state           gRate;
static Float64                  gHostTicksPerFrame = 0.0;
static Float64                  gAnchorHostTime = 0.0;

static _Atomic int              gStreamFormat[kObj_Max];   // avm_format por stream
static _Atomic bool             gStreamActive[kObj_Max];

static avm_engine*              gEngine = NULL;

static os_log_t                 gLog;

// ---------------------------------------------------------------------------
#pragma mark Prototipos de la interfaz

static HRESULT  AVM_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface);
static ULONG    AVM_AddRef(void* inDriver);
static ULONG    AVM_Release(void* inDriver);
static OSStatus AVM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost);
static OSStatus AVM_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID);
static OSStatus AVM_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID);
static OSStatus AVM_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus AVM_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo);
static OSStatus AVM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static OSStatus AVM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo);
static Boolean  AVM_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress);
static OSStatus AVM_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable);
static OSStatus AVM_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize);
static OSStatus AVM_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData);
static OSStatus AVM_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData);
static OSStatus AVM_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus AVM_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID);
static OSStatus AVM_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed);
static OSStatus AVM_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace);
static OSStatus AVM_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);
static OSStatus AVM_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer);
static OSStatus AVM_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo);

static AudioServerPlugInDriverInterface gInterface = {
    NULL,
    AVM_QueryInterface,
    AVM_AddRef,
    AVM_Release,
    AVM_Initialize,
    AVM_CreateDevice,
    AVM_DestroyDevice,
    AVM_AddDeviceClient,
    AVM_RemoveDeviceClient,
    AVM_PerformDeviceConfigurationChange,
    AVM_AbortDeviceConfigurationChange,
    AVM_HasProperty,
    AVM_IsPropertySettable,
    AVM_GetPropertyDataSize,
    AVM_GetPropertyData,
    AVM_SetPropertyData,
    AVM_StartIO,
    AVM_StopIO,
    AVM_GetZeroTimeStamp,
    AVM_WillDoIOOperation,
    AVM_BeginIOOperation,
    AVM_DoIOOperation,
    AVM_EndIOOperation
};
static AudioServerPlugInDriverInterface* gInterfacePtr = &gInterface;
static AudioServerPlugInDriverRef        gDriverRef = &gInterfacePtr;

// ---------------------------------------------------------------------------
#pragma mark Utilidades

static CFStringRef AVM_CopyCFString(const char* s)
{
    return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
}

static int AVM_DeviceIndex(AudioObjectID obj)
{
    for (int i = 0; i < kAVM_NumDevices; i++)
        if (kDevices[i].device == obj) return i;
    return -1;
}

// Devuelve el índice del dispositivo dueño del stream; *outIsInput indica la dirección.
static int AVM_StreamIndex(AudioObjectID obj, bool* outIsInput)
{
    for (int i = 0; i < kAVM_NumDevices; i++) {
        if (kDevices[i].inStream != 0 && kDevices[i].inStream == obj) { if (outIsInput) *outIsInput = true; return i; }
        if (kDevices[i].outStream == obj) { if (outIsInput) *outIsInput = false; return i; }
    }
    return -1;
}

static UInt32 AVM_StreamChannels(int dev, bool isInput)
{
    return isInput ? kDevices[dev].inChannels : kDevices[dev].outChannels;
}

static bool AVM_IsSupportedRate(Float64 r)
{
    return avm_rate_is_supported(r);
}

static UInt64 AVM_NowNs(void)
{
    static mach_timebase_info_data_t tb;
    if (tb.denom == 0) mach_timebase_info(&tb);
    return (UInt64)((__uint128_t)mach_absolute_time() * tb.numer / tb.denom);
}

static Float64 AVM_CurrentRate(void)
{
    pthread_mutex_lock(&gClockMutex);
    Float64 r = gRate.current_rate;
    pthread_mutex_unlock(&gClockMutex);
    return r;
}

static void AVM_FillASBD(AudioStreamBasicDescription* d, avm_format fmt, Float64 rate, UInt32 channels)
{
    UInt32 bps = avm_format_bytes_per_sample(fmt);
    memset(d, 0, sizeof(*d));
    d->mSampleRate       = rate;
    d->mFormatID         = kAudioFormatLinearPCM;
    d->mFormatFlags      = (fmt == AVM_FMT_FLOAT32 ? kAudioFormatFlagIsFloat : kAudioFormatFlagIsSignedInteger)
                           | kAudioFormatFlagsNativeEndian | kAudioFormatFlagIsPacked;
    d->mBytesPerPacket   = bps * channels;
    d->mFramesPerPacket  = 1;
    d->mBytesPerFrame    = bps * channels;
    d->mChannelsPerFrame = channels;
    d->mBitsPerChannel   = avm_format_bits(fmt);
}

// Identifica cuál de nuestros formatos describe un ASBD; -1 si ninguno.
static int AVM_FormatFromASBD(const AudioStreamBasicDescription* d, UInt32 channels)
{
    if (d->mFormatID != kAudioFormatLinearPCM || d->mChannelsPerFrame != channels || d->mFramesPerPacket != 1)
        return -1;
    for (int f = 0; f < AVM_FMT_COUNT; f++) {
        AudioStreamBasicDescription ref;
        AVM_FillASBD(&ref, (avm_format)f, d->mSampleRate, channels);
        UInt32 mask = kAudioFormatFlagIsFloat | kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsBigEndian | kAudioFormatFlagIsPacked | kAudioFormatFlagIsNonInterleaved;
        if ((d->mFormatFlags & mask) == (ref.mFormatFlags & mask)
            && d->mBitsPerChannel == ref.mBitsPerChannel
            && d->mBytesPerFrame == ref.mBytesPerFrame)
            return f;
    }
    return -1;
}

static void AVM_UpdateTicksPerFrame_Locked(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    Float64 hostClockFrequency = ((Float64)tb.denom / (Float64)tb.numer) * 1000000000.0;
    gHostTicksPerFrame = hostClockFrequency / gRate.current_rate;
}

static void AVM_NotifyAllDevices(AudioObjectPropertySelector sel)
{
    if (gHost == NULL) return;
    AudioObjectPropertyAddress a = { sel, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    gHost->PropertiesChanged(gHost, kObj_PlugIn, 1, &a);
    for (int i = 0; i < kAVM_NumDevices; i++)
        gHost->PropertiesChanged(gHost, kDevices[i].device, 1, &a);
}

// ---------------------------------------------------------------------------
#pragma mark Persistencia

static void AVM_StoreNumber(const char* key, CFNumberType type, const void* value)
{
    if (gHost == NULL) return;
    CFStringRef k = AVM_CopyCFString(key);
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, type, value);
    if (k && n) gHost->WriteToStorage(gHost, k, n);
    if (n) CFRelease(n);
    if (k) CFRelease(k);
}

static bool AVM_LoadNumber(const char* key, CFNumberType type, void* outValue)
{
    if (gHost == NULL) return false;
    bool ok = false;
    CFStringRef k = AVM_CopyCFString(key);
    CFPropertyListRef v = NULL;
    if (k && gHost->CopyFromStorage(gHost, k, &v) == 0 && v != NULL) {
        if (CFGetTypeID(v) == CFNumberGetTypeID())
            ok = CFNumberGetValue((CFNumberRef)v, type, outValue);
        CFRelease(v);
    }
    if (k) CFRelease(k);
    return ok;
}

static void AVM_StoreMatrix(const avm_matrix* m)
{
    if (gHost == NULL) return;
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, (const UInt8*)m->cell, sizeof(m->cell));
    if (d) {
        gHost->WriteToStorage(gHost, CFSTR("matrix"), d);
        CFRelease(d);
    }
}

static void AVM_StoreMixer(const avm_mixer* mx)
{
    if (gHost == NULL) return;
    UInt8 blob[AVM_MIXER_BLOB_SIZE];
    avm_mixer_pack(mx, blob);
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, blob, sizeof(blob));
    if (d) {
        gHost->WriteToStorage(gHost, CFSTR("mixer"), d);
        CFRelease(d);
    }
}

static void AVM_LoadState(void)
{
    Float64 rate = 0;
    if (AVM_LoadNumber("sample_rate", kCFNumberFloat64Type, &rate) && AVM_IsSupportedRate(rate))
        gRate.current_rate = rate;

    for (int i = 0; i < kAVM_NumDevices; i++) {
        AudioObjectID streams[2] = { kDevices[i].inStream, kDevices[i].outStream };
        for (int s = 0; s < 2; s++) {
            if (streams[s] == 0) continue;
            char key[32];
            snprintf(key, sizeof(key), "format_stream_%u", (unsigned)streams[s]);
            SInt32 f = 0;
            if (AVM_LoadNumber(key, kCFNumberSInt32Type, &f) && f >= 0 && f < AVM_FMT_COUNT)
                atomic_store(&gStreamFormat[streams[s]], f);
        }
    }

    CFPropertyListRef v = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("matrix"), &v) == 0 && v != NULL) {
        if (CFGetTypeID(v) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)v) == (CFIndex)sizeof(avm_matrix)) {
            avm_matrix m;
            memcpy(m.cell, CFDataGetBytePtr((CFDataRef)v), sizeof(m.cell));
            if (avm_matrix_is_valid(&m))
                avm_engine_set_matrix(gEngine, &m);
            else
                os_log_error(gLog, "AVMatrix: matriz guardada inválida; se usa la predeterminada");
        }
        CFRelease(v);
    }

    // Mixer: sin ajustes guardados (instalación nueva), todo a 0 dB y sin mute.
    v = NULL;
    if (gHost->CopyFromStorage(gHost, CFSTR("mixer"), &v) == 0 && v != NULL) {
        avm_mixer mx;
        if (CFGetTypeID(v) == CFDataGetTypeID()
            && avm_mixer_unpack(CFDataGetBytePtr((CFDataRef)v), (size_t)CFDataGetLength((CFDataRef)v), &mx))
            avm_engine_set_mixer(gEngine, &mx);
        else
            os_log_error(gLog, "AVMatrix: mixer guardado inválido; todos los canales a 0 dB sin mute");
        CFRelease(v);
    }
}

// ---------------------------------------------------------------------------
#pragma mark Fábrica e IUnknown

__attribute__((visibility("default"))) void* AVMatrix_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID);

void* AVMatrix_Create(CFAllocatorRef inAllocator, CFUUIDRef inRequestedTypeUUID)
{
    (void)inAllocator;
    if (CFEqual(inRequestedTypeUUID, kAudioServerPlugInTypeUUID))
        return gDriverRef;
    return NULL;
}

static HRESULT AVM_QueryInterface(void* inDriver, REFIID inUUID, LPVOID* outInterface)
{
    if (inDriver != gDriverRef || outInterface == NULL)
        return kAudioHardwareBadObjectError;

    HRESULT result = 0;
    CFUUIDRef requested = CFUUIDCreateFromUUIDBytes(NULL, inUUID);
    if (requested == NULL) return kAudioHardwareIllegalOperationError;

    if (CFEqual(requested, IUnknownUUID) || CFEqual(requested, kAudioServerPlugInDriverInterfaceUUID)) {
        pthread_mutex_lock(&gStateMutex);
        gRefCount++;
        pthread_mutex_unlock(&gStateMutex);
        *outInterface = gDriverRef;
    } else {
        *outInterface = NULL;
        result = E_NOINTERFACE;
    }
    CFRelease(requested);
    return result;
}

static ULONG AVM_AddRef(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount < UINT32_MAX) gRefCount++;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    return r;
}

static ULONG AVM_Release(void* inDriver)
{
    if (inDriver != gDriverRef) return 0;
    pthread_mutex_lock(&gStateMutex);
    if (gRefCount > 0) gRefCount--;
    ULONG r = gRefCount;
    pthread_mutex_unlock(&gStateMutex);
    // El driver es estático y vive mientras el proceso del host exista.
    return r;
}

// ---------------------------------------------------------------------------
#pragma mark Operaciones básicas

static OSStatus AVM_Initialize(AudioServerPlugInDriverRef inDriver, AudioServerPlugInHostRef inHost)
{
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;

    gLog = os_log_create(kAVM_BundleID, "driver");
    gHost = inHost;

    for (int i = 0; i < kObj_Max; i++) {
        atomic_init(&gStreamFormat[i], AVM_FMT_FLOAT32);
        atomic_init(&gStreamActive[i], true);
    }

    avm_rate_init(&gRate, kAVM_NumDevices, 48000.0, kAVM_RateTransactionTimeoutNs);

    gEngine = (avm_engine*)calloc(1, sizeof(avm_engine));
    if (gEngine == NULL) return kAudioHardwareUnspecifiedError;
    avm_engine_init(gEngine);
    avm_persist_init(&gPersistMatrix, &gStateMutex);
    avm_persist_init(&gPersistMixer, &gStateMutex);

    AVM_LoadState();

    pthread_mutex_lock(&gClockMutex);
    AVM_UpdateTicksPerFrame_Locked();
    pthread_mutex_unlock(&gClockMutex);

    os_log(gLog, "AVMatrix: inicializado a %.0f Hz", gRate.current_rate);
    return 0;
}

static OSStatus AVM_CreateDevice(AudioServerPlugInDriverRef inDriver, CFDictionaryRef inDescription, const AudioServerPlugInClientInfo* inClientInfo, AudioObjectID* outDeviceObjectID)
{
    (void)inDriver; (void)inDescription; (void)inClientInfo; (void)outDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AVM_DestroyDevice(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID)
{
    (void)inDriver; (void)inDeviceObjectID;
    return kAudioHardwareUnsupportedOperationError;
}

static OSStatus AVM_AddDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (AVM_DeviceIndex(inDeviceObjectID) < 0) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus AVM_RemoveDeviceClient(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, const AudioServerPlugInClientInfo* inClientInfo)
{
    (void)inClientInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (AVM_DeviceIndex(inDeviceObjectID) < 0) return kAudioHardwareBadObjectError;
    return 0;
}

// Pide a los tres dispositivos la reversión abierta por avm_rate_abort.
static void AVM_IssueRateRevert(avm_rate_abort_result r)
{
    if (!r.revert_needed || gHost == NULL) return;
    os_log_error(gLog, "AVMatrix: cambio de frecuencia cancelado a medias; revirtiendo a %.0f Hz", r.revert_rate);
    for (int i = 0; i < kAVM_NumDevices; i++) {
        OSStatus err = gHost->RequestDeviceConfigurationChange(gHost, kDevices[i].device,
            AVM_ACTION(kAVM_ActionSetRate, r.revert_generation, (UInt32)r.revert_rate), NULL);
        if (err != 0) {
            pthread_mutex_lock(&gClockMutex);
            avm_rate_abort(&gRate, (uint32_t)i, r.revert_generation, AVM_NowNs());  // marca inconsistente
            pthread_mutex_unlock(&gClockMutex);
            os_log_error(gLog, "AVMatrix: la reversión de frecuencia falló (%d); estado inconsistente", (int)err);
            return;
        }
    }
}

// Aplica una acción de configuración autorizada por el host. Ver PerformDeviceConfigurationChange.
static OSStatus AVM_PerformConfigChange(AudioObjectID inDeviceObjectID, UInt64 inChangeAction)
{
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;

    UInt64 kind = AVM_ACTION_KIND(inChangeAction);
    UInt32 gen = AVM_ACTION_GEN(inChangeAction);
    UInt32 value = AVM_ACTION_VALUE(inChangeAction);

    if (kind == kAVM_ActionSetRate) {
        Float64 newRate = (Float64)value;
        pthread_mutex_lock(&gClockMutex);
        avm_rate_status st = avm_rate_perform(&gRate, (uint32_t)dev, gen, newRate);
        avm_rate_abort_result revert = { false, 0.0, 0 };
        bool completed = false;
        if (st == AVM_RATE_OK) {
            // No hay IO en ningún dispositivo (lo garantiza avm_rate_perform);
            // el ancla del reloj se recalcula en el próximo StartIO.
            AVM_UpdateTicksPerFrame_Locked();
            completed = !avm_rate_pending(&gRate) && !gRate.inconsistent;
        } else if (st == AVM_RATE_IO_ACTIVE) {
            // No se aplica con IO activa: se trata como cancelación.
            revert = avm_rate_abort(&gRate, (uint32_t)dev, gen, AVM_NowNs());
        }
        Float64 current = gRate.current_rate;
        pthread_mutex_unlock(&gClockMutex);

        if (st == AVM_RATE_OK) {
            if (completed) {
                AVM_StoreNumber("sample_rate", kCFNumberFloat64Type, &current);
                os_log(gLog, "AVMatrix: frecuencia cambiada a %.0f Hz", current);
            }
            return 0;
        }
        AVM_IssueRateRevert(revert);
        return st == AVM_RATE_IO_ACTIVE ? kAudioHardwareNotReadyError : kAudioHardwareIllegalOperationError;
    }

    if (kind == kAVM_ActionSetFormat) {
        AudioObjectID stream = (AudioObjectID)(value >> 8);
        int fmt = (int)(value & 0xFF);
        bool isInput = false;
        if (stream >= kObj_Max || AVM_StreamIndex(stream, &isInput) != dev || fmt < 0 || fmt >= AVM_FMT_COUNT)
            return kAudioHardwareIllegalOperationError;
        // El host detiene la IO de este dispositivo antes de llamar aquí; el
        // formato solo afecta a este stream.
        atomic_store(&gStreamFormat[stream], fmt);
        char key[32];
        snprintf(key, sizeof(key), "format_stream_%u", (unsigned)stream);
        SInt32 f = fmt;
        AVM_StoreNumber(key, kCFNumberSInt32Type, &f);
        return 0;
    }

    return kAudioHardwareIllegalOperationError;
}

static OSStatus AVM_PerformDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    OSStatus r = AVM_PerformConfigChange(inDeviceObjectID, inChangeAction);
    if (AVM_ACTION_KIND(inChangeAction) == kAVM_ActionSetRate)
        AVM_NotifyAllDevices(kAVM_PropertyRateState);
    return r;
}

static OSStatus AVM_AbortDeviceConfigurationChange(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt64 inChangeAction, void* inChangeInfo)
{
    (void)inChangeInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;

    if (AVM_ACTION_KIND(inChangeAction) == kAVM_ActionSetRate) {
        pthread_mutex_lock(&gClockMutex);
        avm_rate_abort_result r = avm_rate_abort(&gRate, (uint32_t)dev, AVM_ACTION_GEN(inChangeAction), AVM_NowNs());
        pthread_mutex_unlock(&gClockMutex);
        AVM_IssueRateRevert(r);
        AVM_NotifyAllDevices(kAVM_PropertyRateState);
    }
    // Un cambio de formato solo se aplica en Perform: cancelarlo no deja nada que deshacer.
    return 0;
}

// Pide el cambio de frecuencia a los tres dispositivos (comparten reloj).
// Se rechaza si hay IO activa en cualquiera de ellos o si ya hay un cambio en curso.
static OSStatus AVM_RequestRateChange(Float64 rate)
{
    if (!AVM_IsSupportedRate(rate)) return kAudioDeviceUnsupportedFormatError;
    uint32_t gen = 0;
    pthread_mutex_lock(&gClockMutex);
    avm_rate_status st = avm_rate_begin(&gRate, rate, AVM_NowNs(), &gen);
    pthread_mutex_unlock(&gClockMutex);

    switch (st) {
        case AVM_RATE_SAME:        return 0;
        case AVM_RATE_UNSUPPORTED: return kAudioDeviceUnsupportedFormatError;
        case AVM_RATE_IO_ACTIVE:
            os_log(gLog, "AVMatrix: cambio a %.0f Hz rechazado: hay audio activo en un dispositivo AV Matrix", rate);
            return kAudioHardwareNotReadyError;
        case AVM_RATE_BUSY:        return kAudioHardwareNotReadyError;
        case AVM_RATE_STALE:       return kAudioHardwareIllegalOperationError;
        case AVM_RATE_INCONSISTENT: return kAudioHardwareNotReadyError;  // avm_rate_begin no lo devuelve
        case AVM_RATE_OK:          break;
    }

    for (int i = 0; i < kAVM_NumDevices; i++) {
        OSStatus err = gHost->RequestDeviceConfigurationChange(gHost, kDevices[i].device,
            AVM_ACTION(kAVM_ActionSetRate, gen, (UInt32)rate), NULL);
        if (err != 0) {
            os_log_error(gLog, "AVMatrix: el host rechazó el cambio de frecuencia (%d)", (int)err);
            pthread_mutex_lock(&gClockMutex);
            avm_rate_abort_result r = avm_rate_abort(&gRate, (uint32_t)i, gen, AVM_NowNs());
            pthread_mutex_unlock(&gClockMutex);
            AVM_IssueRateRevert(r);
            AVM_NotifyAllDevices(kAVM_PropertyRateState);
            return err;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
#pragma mark Matriz (propiedad personalizada)

static OSStatus AVM_GetMatrixProperty(UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    avm_matrix m;
    // Los escritores de la matriz están serializados por gStateMutex: con él
    // tomado, la lectura acotada siempre encuentra una copia coherente.
    pthread_mutex_lock(&gStateMutex);
    bool ok = avm_engine_try_get_matrix(gEngine, &m);
    pthread_mutex_unlock(&gStateMutex);
    if (!ok) return kAudioHardwareNotReadyError;
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, (const UInt8*)m.cell, sizeof(m.cell));
    if (d == NULL) return kAudioHardwareUnspecifiedError;
    *((CFPropertyListRef*)outData) = d;  // el cliente la libera
    *outDataSize = sizeof(CFPropertyListRef);
    return 0;
}

// Adaptadores para avm_persist_commit.
static bool AVM_ApplyMatrix(void* obj, const void* v) { avm_engine_set_matrix((avm_engine*)obj, (const avm_matrix*)v); return true; }
static void AVM_SnapMatrix(void* obj, void* out)
{
    // Con gStateMutex tomado (escritores serializados) la lectura acotada siempre
    // encuentra una copia coherente; si no, se conserva la copia de `out`.
    avm_engine_try_get_matrix((avm_engine*)obj, (avm_matrix*)out);
}
static void AVM_WriteMatrix(const void* snap, void* ctx) { (void)ctx; AVM_StoreMatrix((const avm_matrix*)snap); }
static bool AVM_ApplyMixer(void* obj, const void* v) { return avm_engine_set_mixer((avm_engine*)obj, (const avm_mixer*)v); }
static void AVM_SnapMixer(void* obj, void* out) { avm_engine_get_mixer((const avm_engine*)obj, (avm_mixer*)out); }
static void AVM_WriteMixer(const void* snap, void* ctx) { (void)ctx; AVM_StoreMixer((const avm_mixer*)snap); }

static OSStatus AVM_SetMatrixProperty(UInt32 inDataSize, const void* inData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    CFPropertyListRef v = *((const CFPropertyListRef*)inData);
    if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID()) return kAudioHardwareIllegalOperationError;
    CFDataRef d = (CFDataRef)v;
    if (CFDataGetLength(d) != (CFIndex)sizeof(avm_matrix)) return kAudioHardwareBadPropertySizeError;

    avm_matrix m;
    memcpy(m.cell, CFDataGetBytePtr(d), sizeof(m.cell));
    // Se rechaza completa cualquier matriz con un cruce prohibido (lazo de realimentación).
    if (!avm_matrix_is_valid(&m)) return kAudioHardwareIllegalOperationError;

    avm_matrix snap = m;
    avm_persist_commit(&gPersistMatrix, AVM_ApplyMatrix, AVM_SnapMatrix, AVM_WriteMatrix, gEngine, &m, &snap, NULL);
    AVM_NotifyAllDevices(kAVM_PropertyMatrix);
    return 0;
}

static OSStatus AVM_GetRateStateProperty(UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    pthread_mutex_lock(&gClockMutex);
    SInt32 health = avm_rate_health(&gRate, AVM_NowNs());
    pthread_mutex_unlock(&gClockMutex);
    CFNumberRef n = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &health);
    if (n == NULL) return kAudioHardwareUnspecifiedError;
    *((CFPropertyListRef*)outData) = n;  // el cliente la libera
    *outDataSize = sizeof(CFPropertyListRef);
    return 0;
}

static OSStatus AVM_GetMetersProperty(UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    avm_meter_reading src[AVM_NUM_SOURCES], dst[AVM_NUM_DESTS];
    avm_engine_meter_snapshot(gEngine, AVM_NowNs(), src, dst);
    UInt8 blob[AVM_METERS_BLOB_SIZE];
    avm_meters_pack(src, dst, blob);
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, blob, sizeof(blob));
    if (d == NULL) return kAudioHardwareUnspecifiedError;
    *((CFPropertyListRef*)outData) = d;  // el cliente la libera
    *outDataSize = sizeof(CFPropertyListRef);
    return 0;
}

static OSStatus AVM_GetMixerProperty(UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    avm_mixer mx;
    pthread_mutex_lock(&gStateMutex);
    avm_engine_get_mixer(gEngine, &mx);
    pthread_mutex_unlock(&gStateMutex);
    UInt8 blob[AVM_MIXER_BLOB_SIZE];
    avm_mixer_pack(&mx, blob);
    CFDataRef d = CFDataCreate(kCFAllocatorDefault, blob, sizeof(blob));
    if (d == NULL) return kAudioHardwareUnspecifiedError;
    *((CFPropertyListRef*)outData) = d;  // el cliente la libera
    *outDataSize = sizeof(CFPropertyListRef);
    return 0;
}

// Solo publica las ganancias objetivo (almacenamientos atómicos): el hilo de
// audio nunca espera. Cada canal llega al nuevo nivel con una rampa corta.
static OSStatus AVM_SetMixerProperty(UInt32 inDataSize, const void* inData)
{
    if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
    CFPropertyListRef v = *((const CFPropertyListRef*)inData);
    if (v == NULL || CFGetTypeID(v) != CFDataGetTypeID()) return kAudioHardwareIllegalOperationError;
    CFDataRef d = (CFDataRef)v;
    if (CFDataGetLength(d) != (CFIndex)AVM_MIXER_BLOB_SIZE) return kAudioHardwareBadPropertySizeError;
    avm_mixer mx;
    if (!avm_mixer_unpack(CFDataGetBytePtr(d), AVM_MIXER_BLOB_SIZE, &mx)) return kAudioHardwareIllegalOperationError;

    avm_mixer snap = mx;
    if (!avm_persist_commit(&gPersistMixer, AVM_ApplyMixer, AVM_SnapMixer, AVM_WriteMixer, gEngine, &mx, &snap, NULL))
        return kAudioHardwareIllegalOperationError;
    AVM_NotifyAllDevices(kAVM_PropertyMixer);
    return 0;
}

static const AudioServerPlugInCustomPropertyInfo kCustomProps[] = {
    { kAVM_PropertyMatrix,    kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kAVM_PropertyRateState, kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kAVM_PropertyMeters,    kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone },
    { kAVM_PropertyMixer,     kAudioServerPlugInCustomPropertyDataTypeCFPropertyList, kAudioServerPlugInCustomPropertyDataTypeNone }
};

// ---------------------------------------------------------------------------
#pragma mark Propiedades del plug-in

static Boolean AVM_PlugIn_HasProperty(const AudioObjectPropertyAddress* a)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:
        case kAudioPlugInPropertyTranslateUIDToDevice:
        case kAudioPlugInPropertyResourceBundle:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAVM_PropertyMatrix:
        case kAVM_PropertyRateState:
        case kAVM_PropertyMeters:
        case kAVM_PropertyMixer:
            return true;
    }
    return false;
}

static OSStatus AVM_PlugIn_GetPropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* outDataSize)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:              *outDataSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:              *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyManufacturer:       *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList:         *outDataSize = kAVM_NumDevices * sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyTranslateUIDToDevice: *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioPlugInPropertyResourceBundle:     *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyCustomPropertyInfoList: *outDataSize = sizeof(kCustomProps); return 0;
        case kAVM_PropertyMatrix:
        case kAVM_PropertyRateState:
        case kAVM_PropertyMeters:
        case kAVM_PropertyMixer:                     *outDataSize = sizeof(CFPropertyListRef); return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus AVM_PlugIn_GetPropertyData(const AudioObjectPropertyAddress* a, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *((AudioClassID*)outData) = kAudioObjectClassID; *outDataSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyClass:
            if (inDataSize < sizeof(AudioClassID)) return kAudioHardwareBadPropertySizeError;
            *((AudioClassID*)outData) = kAudioPlugInClassID; *outDataSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            *((AudioObjectID*)outData) = kAudioObjectUnknown; *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyManufacturer:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *((CFStringRef*)outData) = AVM_CopyCFString(kAVM_Manufacturer); *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioPlugInPropertyDeviceList: {
            UInt32 n = inDataSize / sizeof(AudioObjectID);
            if (n > kAVM_NumDevices) n = kAVM_NumDevices;
            for (UInt32 i = 0; i < n; i++) ((AudioObjectID*)outData)[i] = kDevices[i].device;
            *outDataSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyTranslateUIDToDevice: {
            if (inDataSize < sizeof(AudioObjectID)) return kAudioHardwareBadPropertySizeError;
            if (inQualifierDataSize != sizeof(CFStringRef) || inQualifierData == NULL) return kAudioHardwareBadPropertySizeError;
            CFStringRef uid = *((const CFStringRef*)inQualifierData);
            AudioObjectID found = kAudioObjectUnknown;
            for (int i = 0; i < kAVM_NumDevices && uid != NULL; i++) {
                CFStringRef u = AVM_CopyCFString(kDevices[i].uid);
                if (u && CFStringCompare(uid, u, 0) == kCFCompareEqualTo) found = kDevices[i].device;
                if (u) CFRelease(u);
            }
            *((AudioObjectID*)outData) = found; *outDataSize = sizeof(AudioObjectID);
            return 0;
        }
        case kAudioPlugInPropertyResourceBundle:
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            *((CFStringRef*)outData) = CFSTR(""); *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyCustomPropertyInfoList: {
            UInt32 n = inDataSize / sizeof(AudioServerPlugInCustomPropertyInfo);
            UInt32 total = sizeof(kCustomProps) / sizeof(kCustomProps[0]);
            if (n > total) n = total;
            memcpy(outData, kCustomProps, n * sizeof(AudioServerPlugInCustomPropertyInfo));
            *outDataSize = n * sizeof(AudioServerPlugInCustomPropertyInfo);
            return 0;
        }
        case kAVM_PropertyMatrix:
            return AVM_GetMatrixProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyRateState:
            return AVM_GetRateStateProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyMeters:
            return AVM_GetMetersProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyMixer:
            return AVM_GetMixerProperty(inDataSize, outDataSize, outData);
    }
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
#pragma mark Propiedades del dispositivo

static Boolean AVM_Device_HasProperty(int dev, const AudioObjectPropertyAddress* a)
{
    const AVM_DeviceDesc* d = &kDevices[dev];
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyControlList:
        case kAudioObjectPropertyCustomPropertyInfoList:
        case kAVM_PropertyMatrix:
        case kAVM_PropertyRateState:
        case kAVM_PropertyMeters:
        case kAVM_PropertyMixer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyRelatedDevices:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertyStreams:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyNominalSampleRate:
        case kAudioDevicePropertyAvailableNominalSampleRates:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:
            return true;
        case kAudioDevicePropertyPreferredChannelsForStereo:
        case kAudioDevicePropertyPreferredChannelLayout:
            if (a->mScope == kAudioObjectPropertyScopeInput) return d->inChannels > 0;
            if (a->mScope == kAudioObjectPropertyScopeOutput) return d->outChannels > 0;
            return false;
        case kAudioObjectPropertyElementName: {
            UInt32 n = (a->mScope == kAudioObjectPropertyScopeInput) ? d->inChannels
                     : (a->mScope == kAudioObjectPropertyScopeOutput) ? d->outChannels : 0;
            return a->mElement >= 1 && a->mElement <= n;
        }
    }
    return false;
}

static UInt32 AVM_Device_StreamCount(int dev, AudioObjectPropertyScope scope)
{
    const AVM_DeviceDesc* d = &kDevices[dev];
    UInt32 in = d->inStream ? 1 : 0, out = 1;
    if (scope == kAudioObjectPropertyScopeInput) return in;
    if (scope == kAudioObjectPropertyScopeOutput) return out;
    return in + out;
}

static UInt32 AVM_Device_FillStreams(int dev, AudioObjectPropertyScope scope, AudioObjectID* out, UInt32 max)
{
    const AVM_DeviceDesc* d = &kDevices[dev];
    UInt32 n = 0;
    if ((scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeInput) && d->inStream && n < max)
        out[n++] = d->inStream;
    if ((scope == kAudioObjectPropertyScopeGlobal || scope == kAudioObjectPropertyScopeOutput) && n < max)
        out[n++] = d->outStream;
    return n;
}

static UInt32 AVM_Device_LayoutChannels(int dev, AudioObjectPropertyScope scope)
{
    return (scope == kAudioObjectPropertyScopeInput) ? kDevices[dev].inChannels : kDevices[dev].outChannels;
}

static OSStatus AVM_Device_GetPropertyDataSize(int dev, const AudioObjectPropertyAddress* a, UInt32* outDataSize)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:                 *outDataSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:                 *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyName:
        case kAudioObjectPropertyManufacturer:
        case kAudioDevicePropertyDeviceUID:
        case kAudioDevicePropertyModelUID:
        case kAudioObjectPropertyElementName:           *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams:               *outDataSize = AVM_Device_StreamCount(dev, a->mScope) * sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyControlList:           *outDataSize = 0; return 0;
        case kAudioObjectPropertyCustomPropertyInfoList: *outDataSize = sizeof(kCustomProps); return 0;
        case kAVM_PropertyMatrix:
        case kAVM_PropertyRateState:
        case kAVM_PropertyMeters:
        case kAVM_PropertyMixer:                        *outDataSize = sizeof(CFPropertyListRef); return 0;
        case kAudioDevicePropertyTransportType:
        case kAudioDevicePropertyClockDomain:
        case kAudioDevicePropertyDeviceIsAlive:
        case kAudioDevicePropertyDeviceIsRunning:
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:
        case kAudioDevicePropertyLatency:
        case kAudioDevicePropertySafetyOffset:
        case kAudioDevicePropertyIsHidden:
        case kAudioDevicePropertyZeroTimeStampPeriod:
        case kAudioDevicePropertyClockIsStable:         *outDataSize = sizeof(UInt32); return 0;
        case kAudioDevicePropertyRelatedDevices:        *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyNominalSampleRate:     *outDataSize = sizeof(Float64); return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates: *outDataSize = kAVM_NumSampleRates * sizeof(AudioValueRange); return 0;
        case kAudioDevicePropertyPreferredChannelsForStereo:  *outDataSize = 2 * sizeof(UInt32); return 0;
        case kAudioDevicePropertyPreferredChannelLayout:
            *outDataSize = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + AVM_Device_LayoutChannels(dev, a->mScope) * sizeof(AudioChannelDescription));
            return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus AVM_Device_GetPropertyData(int dev, const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    const AVM_DeviceDesc* d = &kDevices[dev];

#define RETURN_U32(v) do { if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError; \
        *((UInt32*)outData) = (UInt32)(v); *outDataSize = sizeof(UInt32); return 0; } while (0)
#define RETURN_STR(cstr) do { if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError; \
        *((CFStringRef*)outData) = AVM_CopyCFString(cstr); *outDataSize = sizeof(CFStringRef); return 0; } while (0)

    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:     RETURN_U32(kAudioObjectClassID);
        case kAudioObjectPropertyClass:         RETURN_U32(kAudioDeviceClassID);
        case kAudioObjectPropertyOwner:         RETURN_U32(kObj_PlugIn);
        case kAudioObjectPropertyName:          RETURN_STR(d->name);
        case kAudioObjectPropertyManufacturer:  RETURN_STR(kAVM_Manufacturer);
        case kAudioDevicePropertyDeviceUID:     RETURN_STR(d->uid);
        case kAudioDevicePropertyModelUID:      RETURN_STR(d->modelUID);
        case kAudioObjectPropertyElementName: {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            const char* pattern = (a->mScope == kAudioObjectPropertyScopeInput) ? d->inChannelName : d->outChannelName;
            UInt32 n = (a->mScope == kAudioObjectPropertyScopeInput) ? d->inChannels
                     : (a->mScope == kAudioObjectPropertyScopeOutput) ? d->outChannels : 0;
            if (pattern == NULL || a->mElement < 1 || a->mElement > n) return kAudioHardwareUnknownPropertyError;
            char buf[64];
            if (d->srcGroup == AVM_SRC_GROUP_MUSIC)
                snprintf(buf, sizeof(buf), "%s", a->mElement == 1 ? "Música L" : "Música R");
            else if (d->device == kObj_Dev_Smaart && a->mScope == kAudioObjectPropertyScopeInput && a->mElement == 8)
                snprintf(buf, sizeof(buf), "Entrada 8 (referencia local)");  // así la ve Smaart
            else
                snprintf(buf, sizeof(buf), pattern, (unsigned)a->mElement);
            *((CFStringRef*)outData) = AVM_CopyCFString(buf);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        }
        case kAudioObjectPropertyOwnedObjects:
        case kAudioDevicePropertyStreams: {
            UInt32 n = AVM_Device_FillStreams(dev, a->mScope, (AudioObjectID*)outData, inDataSize / sizeof(AudioObjectID));
            *outDataSize = n * sizeof(AudioObjectID);
            return 0;
        }
        case kAudioObjectPropertyControlList:
            *outDataSize = 0;
            return 0;
        case kAudioObjectPropertyCustomPropertyInfoList:
            return AVM_PlugIn_GetPropertyData(a, 0, NULL, inDataSize, outDataSize, outData);
        case kAVM_PropertyMatrix:
            return AVM_GetMatrixProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyRateState:
            return AVM_GetRateStateProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyMeters:
            return AVM_GetMetersProperty(inDataSize, outDataSize, outData);
        case kAVM_PropertyMixer:
            return AVM_GetMixerProperty(inDataSize, outDataSize, outData);
        case kAudioDevicePropertyTransportType: RETURN_U32(kAudioDeviceTransportTypeVirtual);
        case kAudioDevicePropertyRelatedDevices:
            if (inDataSize < sizeof(AudioObjectID)) { *outDataSize = 0; return 0; }
            *((AudioObjectID*)outData) = d->device; *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioDevicePropertyClockDomain:   RETURN_U32(kAVM_ClockDomain);
        case kAudioDevicePropertyDeviceIsAlive: RETURN_U32(1);
        case kAudioDevicePropertyDeviceIsRunning: {
            pthread_mutex_lock(&gClockMutex);
            UInt32 running = gRate.running[dev] > 0;
            pthread_mutex_unlock(&gClockMutex);
            RETURN_U32(running);
        }
        case kAudioDevicePropertyDeviceCanBeDefaultDevice:
            // Smaart y LISTENTO no pueden ser la salida/entrada por defecto del sistema:
            // así los sonidos de otras aplicaciones no se mezclan por accidente con la medición.
            RETURN_U32(d->canBeDefaultOutput && a->mScope != kAudioObjectPropertyScopeInput);
        case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice: RETURN_U32(0);
        case kAudioDevicePropertyLatency:       RETURN_U32(0);
        case kAudioDevicePropertySafetyOffset:  RETURN_U32(0);
        case kAudioDevicePropertyIsHidden:      RETURN_U32(0);
        case kAudioDevicePropertyZeroTimeStampPeriod: RETURN_U32(kAVM_ZeroTimeStampPeriod);
        case kAudioDevicePropertyClockIsStable: RETURN_U32(1);
        case kAudioDevicePropertyNominalSampleRate: {
            if (inDataSize < sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            *((Float64*)outData) = AVM_CurrentRate();
            *outDataSize = sizeof(Float64);
            return 0;
        }
        case kAudioDevicePropertyAvailableNominalSampleRates: {
            UInt32 n = inDataSize / sizeof(AudioValueRange);
            if (n > kAVM_NumSampleRates) n = kAVM_NumSampleRates;
            for (UInt32 i = 0; i < n; i++) {
                ((AudioValueRange*)outData)[i].mMinimum = kAVM_SampleRates[i];
                ((AudioValueRange*)outData)[i].mMaximum = kAVM_SampleRates[i];
            }
            *outDataSize = n * sizeof(AudioValueRange);
            return 0;
        }
        case kAudioDevicePropertyPreferredChannelsForStereo:
            if (inDataSize < 2 * sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
            ((UInt32*)outData)[0] = 1;
            ((UInt32*)outData)[1] = 2;
            *outDataSize = 2 * sizeof(UInt32);
            return 0;
        case kAudioDevicePropertyPreferredChannelLayout: {
            UInt32 nch = AVM_Device_LayoutChannels(dev, a->mScope);
            UInt32 size = (UInt32)(offsetof(AudioChannelLayout, mChannelDescriptions) + nch * sizeof(AudioChannelDescription));
            if (inDataSize < size) return kAudioHardwareBadPropertySizeError;
            AudioChannelLayout* l = (AudioChannelLayout*)outData;
            memset(l, 0, size);
            l->mChannelLayoutTag = kAudioChannelLayoutTag_UseChannelDescriptions;
            l->mNumberChannelDescriptions = nch;
            for (UInt32 i = 0; i < nch; i++) {
                if (d->srcGroup == AVM_SRC_GROUP_MUSIC)
                    l->mChannelDescriptions[i].mChannelLabel = (i == 0) ? kAudioChannelLabel_Left : kAudioChannelLabel_Right;
                else
                    l->mChannelDescriptions[i].mChannelLabel = kAudioChannelLabel_Discrete_0 + i;
            }
            *outDataSize = size;
            return 0;
        }
    }
#undef RETURN_U32
#undef RETURN_STR
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
#pragma mark Propiedades del stream

static Boolean AVM_Stream_HasProperty(const AudioObjectPropertyAddress* a)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:
        case kAudioObjectPropertyOwner:
        case kAudioObjectPropertyOwnedObjects:
        case kAudioObjectPropertyName:
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:
        case kAudioStreamPropertyAvailableVirtualFormats:
        case kAudioStreamPropertyAvailablePhysicalFormats:
            return true;
    }
    return false;
}

static OSStatus AVM_Stream_GetPropertyDataSize(const AudioObjectPropertyAddress* a, UInt32* outDataSize)
{
    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:
        case kAudioObjectPropertyClass:             *outDataSize = sizeof(AudioClassID); return 0;
        case kAudioObjectPropertyOwner:             *outDataSize = sizeof(AudioObjectID); return 0;
        case kAudioObjectPropertyOwnedObjects:      *outDataSize = 0; return 0;
        case kAudioObjectPropertyName:              *outDataSize = sizeof(CFStringRef); return 0;
        case kAudioStreamPropertyIsActive:
        case kAudioStreamPropertyDirection:
        case kAudioStreamPropertyTerminalType:
        case kAudioStreamPropertyStartingChannel:
        case kAudioStreamPropertyLatency:           *outDataSize = sizeof(UInt32); return 0;
        case kAudioStreamPropertyVirtualFormat:
        case kAudioStreamPropertyPhysicalFormat:    *outDataSize = sizeof(AudioStreamBasicDescription); return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:
            *outDataSize = kAVM_NumSampleRates * sizeof(AudioStreamRangedDescription); return 0;
        case kAudioStreamPropertyAvailablePhysicalFormats:
            *outDataSize = kAVM_NumSampleRates * AVM_FMT_COUNT * sizeof(AudioStreamRangedDescription); return 0;
    }
    return kAudioHardwareUnknownPropertyError;
}

static OSStatus AVM_Stream_GetPropertyData(AudioObjectID stream, int dev, bool isInput, const AudioObjectPropertyAddress* a, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    UInt32 channels = AVM_StreamChannels(dev, isInput);
    Float64 rate = AVM_CurrentRate();

#define RETURN_U32(v) do { if (inDataSize < sizeof(UInt32)) return kAudioHardwareBadPropertySizeError; \
        *((UInt32*)outData) = (UInt32)(v); *outDataSize = sizeof(UInt32); return 0; } while (0)

    switch (a->mSelector) {
        case kAudioObjectPropertyBaseClass:     RETURN_U32(kAudioObjectClassID);
        case kAudioObjectPropertyClass:         RETURN_U32(kAudioStreamClassID);
        case kAudioObjectPropertyOwner:         RETURN_U32(kDevices[dev].device);
        case kAudioObjectPropertyOwnedObjects:  *outDataSize = 0; return 0;
        case kAudioObjectPropertyName: {
            if (inDataSize < sizeof(CFStringRef)) return kAudioHardwareBadPropertySizeError;
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s", kDevices[dev].name, isInput ? "entradas" : "salidas");
            *((CFStringRef*)outData) = AVM_CopyCFString(buf);
            *outDataSize = sizeof(CFStringRef);
            return 0;
        }
        case kAudioStreamPropertyIsActive:      RETURN_U32(atomic_load(&gStreamActive[stream]) ? 1 : 0);
        case kAudioStreamPropertyDirection:     RETURN_U32(isInput ? 1 : 0);
        // Terminal de línea: entradas y salidas genéricas, no micrófono ni altavoz.
        case kAudioStreamPropertyTerminalType:  RETURN_U32(kAudioStreamTerminalTypeLine);
        case kAudioStreamPropertyStartingChannel: RETURN_U32(1);
        case kAudioStreamPropertyLatency:       RETURN_U32(0);
        case kAudioStreamPropertyVirtualFormat:
            if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            AVM_FillASBD((AudioStreamBasicDescription*)outData, AVM_FMT_FLOAT32, rate, channels);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return 0;
        case kAudioStreamPropertyPhysicalFormat:
            if (inDataSize < sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
            AVM_FillASBD((AudioStreamBasicDescription*)outData, (avm_format)atomic_load(&gStreamFormat[stream]), rate, channels);
            *outDataSize = sizeof(AudioStreamBasicDescription);
            return 0;
        case kAudioStreamPropertyAvailableVirtualFormats: {
            UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
            if (n > kAVM_NumSampleRates) n = kAVM_NumSampleRates;
            AudioStreamRangedDescription* r = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < n; i++) {
                AVM_FillASBD(&r[i].mFormat, AVM_FMT_FLOAT32, kAVM_SampleRates[i], channels);
                r[i].mSampleRateRange.mMinimum = kAVM_SampleRates[i];
                r[i].mSampleRateRange.mMaximum = kAVM_SampleRates[i];
            }
            *outDataSize = n * sizeof(AudioStreamRangedDescription);
            return 0;
        }
        case kAudioStreamPropertyAvailablePhysicalFormats: {
            UInt32 n = inDataSize / sizeof(AudioStreamRangedDescription);
            UInt32 total = kAVM_NumSampleRates * AVM_FMT_COUNT;
            if (n > total) n = total;
            AudioStreamRangedDescription* r = (AudioStreamRangedDescription*)outData;
            for (UInt32 i = 0; i < n; i++) {
                avm_format f = (avm_format)(i / kAVM_NumSampleRates);
                Float64 sr = kAVM_SampleRates[i % kAVM_NumSampleRates];
                AVM_FillASBD(&r[i].mFormat, f, sr, channels);
                r[i].mSampleRateRange.mMinimum = sr;
                r[i].mSampleRateRange.mMaximum = sr;
            }
            *outDataSize = n * sizeof(AudioStreamRangedDescription);
            return 0;
        }
    }
#undef RETURN_U32
    return kAudioHardwareUnknownPropertyError;
}

// ---------------------------------------------------------------------------
#pragma mark Despacho de propiedades

static Boolean AVM_HasProperty(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress)
{
    (void)inClientProcessID;
    if (inDriver != gDriverRef || inAddress == NULL) return false;
    if (inObjectID == kObj_PlugIn) return AVM_PlugIn_HasProperty(inAddress);
    int dev = AVM_DeviceIndex(inObjectID);
    if (dev >= 0) return AVM_Device_HasProperty(dev, inAddress);
    if (AVM_StreamIndex(inObjectID, NULL) >= 0) return AVM_Stream_HasProperty(inAddress);
    return false;
}

static OSStatus AVM_IsPropertySettable(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, Boolean* outIsSettable)
{
    (void)inClientProcessID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outIsSettable == NULL) return kAudioHardwareIllegalOperationError;
    if (!AVM_HasProperty(inDriver, inObjectID, inClientProcessID, inAddress)) return kAudioHardwareUnknownPropertyError;

    *outIsSettable = false;
    if (inAddress->mSelector == kAVM_PropertyMatrix || inAddress->mSelector == kAVM_PropertyRateState
        || inAddress->mSelector == kAVM_PropertyMixer) {
        *outIsSettable = true;
    } else if (AVM_DeviceIndex(inObjectID) >= 0) {
        *outIsSettable = (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate);
    } else if (AVM_StreamIndex(inObjectID, NULL) >= 0) {
        switch (inAddress->mSelector) {
            case kAudioStreamPropertyIsActive:
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat:
                *outIsSettable = true;
                break;
        }
    }
    return 0;
}

static OSStatus AVM_GetPropertyDataSize(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32* outDataSize)
{
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outDataSize == NULL) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObj_PlugIn) return AVM_PlugIn_GetPropertyDataSize(inAddress, outDataSize);
    int dev = AVM_DeviceIndex(inObjectID);
    if (dev >= 0) {
        if (!AVM_Device_HasProperty(dev, inAddress)) return kAudioHardwareUnknownPropertyError;
        return AVM_Device_GetPropertyDataSize(dev, inAddress, outDataSize);
    }
    if (AVM_StreamIndex(inObjectID, NULL) >= 0) return AVM_Stream_GetPropertyDataSize(inAddress, outDataSize);
    return kAudioHardwareBadObjectError;
}

static OSStatus AVM_GetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, UInt32* outDataSize, void* outData)
{
    (void)inClientProcessID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || outDataSize == NULL) return kAudioHardwareIllegalOperationError;
    if (outData == NULL && inDataSize > 0) return kAudioHardwareIllegalOperationError;
    if (inObjectID == kObj_PlugIn)
        return AVM_PlugIn_GetPropertyData(inAddress, inQualifierDataSize, inQualifierData, inDataSize, outDataSize, outData);
    int dev = AVM_DeviceIndex(inObjectID);
    if (dev >= 0) {
        if (!AVM_Device_HasProperty(dev, inAddress)) return kAudioHardwareUnknownPropertyError;
        return AVM_Device_GetPropertyData(dev, inAddress, inDataSize, outDataSize, outData);
    }
    bool isInput = false;
    dev = AVM_StreamIndex(inObjectID, &isInput);
    if (dev >= 0) return AVM_Stream_GetPropertyData(inObjectID, dev, isInput, inAddress, inDataSize, outDataSize, outData);
    return kAudioHardwareBadObjectError;
}

static OSStatus AVM_SetPropertyData(AudioServerPlugInDriverRef inDriver, AudioObjectID inObjectID, pid_t inClientProcessID, const AudioObjectPropertyAddress* inAddress, UInt32 inQualifierDataSize, const void* inQualifierData, UInt32 inDataSize, const void* inData)
{
    (void)inClientProcessID; (void)inQualifierDataSize; (void)inQualifierData;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (inAddress == NULL || inData == NULL) return kAudioHardwareIllegalOperationError;

    if (inAddress->mSelector == kAVM_PropertyMatrix
        && (inObjectID == kObj_PlugIn || AVM_DeviceIndex(inObjectID) >= 0))
        return AVM_SetMatrixProperty(inDataSize, inData);

    if (inAddress->mSelector == kAVM_PropertyMixer
        && (inObjectID == kObj_PlugIn || AVM_DeviceIndex(inObjectID) >= 0))
        return AVM_SetMixerProperty(inDataSize, inData);

    if (inAddress->mSelector == kAVM_PropertyRateState
        && (inObjectID == kObj_PlugIn || AVM_DeviceIndex(inObjectID) >= 0)) {
        if (inDataSize < sizeof(CFPropertyListRef)) return kAudioHardwareBadPropertySizeError;
        CFPropertyListRef v = *((const CFPropertyListRef*)inData);
        Float64 rate = 0;
        if (v == NULL || CFGetTypeID(v) != CFNumberGetTypeID()
            || !CFNumberGetValue((CFNumberRef)v, kCFNumberFloat64Type, &rate))
            return kAudioHardwareIllegalOperationError;
        if (rate == 0) rate = AVM_CurrentRate();
        return AVM_RequestRateChange(rate);
    }

    int dev = AVM_DeviceIndex(inObjectID);
    if (dev >= 0) {
        if (inAddress->mSelector == kAudioDevicePropertyNominalSampleRate) {
            if (inDataSize != sizeof(Float64)) return kAudioHardwareBadPropertySizeError;
            return AVM_RequestRateChange(*((const Float64*)inData));
        }
        return AVM_Device_HasProperty(dev, inAddress) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareUnknownPropertyError;
    }

    bool isInput = false;
    dev = AVM_StreamIndex(inObjectID, &isInput);
    if (dev >= 0) {
        switch (inAddress->mSelector) {
            case kAudioStreamPropertyIsActive: {
                if (inDataSize != sizeof(UInt32)) return kAudioHardwareBadPropertySizeError;
                bool active = *((const UInt32*)inData) != 0;
                atomic_store(&gStreamActive[inObjectID], active);
                AudioObjectPropertyAddress ad = { kAudioStreamPropertyIsActive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                gHost->PropertiesChanged(gHost, inObjectID, 1, &ad);
                return 0;
            }
            case kAudioStreamPropertyVirtualFormat:
            case kAudioStreamPropertyPhysicalFormat: {
                if (inDataSize != sizeof(AudioStreamBasicDescription)) return kAudioHardwareBadPropertySizeError;
                const AudioStreamBasicDescription* asbd = (const AudioStreamBasicDescription*)inData;
                UInt32 channels = AVM_StreamChannels(dev, isInput);
                int fmt = AVM_FormatFromASBD(asbd, channels);
                if (fmt < 0 || !AVM_IsSupportedRate(asbd->mSampleRate)) return kAudioDeviceUnsupportedFormatError;
                // El formato virtual (el que ven las aplicaciones) es siempre Float32.
                if (inAddress->mSelector == kAudioStreamPropertyVirtualFormat && fmt != AVM_FMT_FLOAT32)
                    return kAudioDeviceUnsupportedFormatError;
                OSStatus err = AVM_RequestRateChange(asbd->mSampleRate);
                if (err) return err;
                if (inAddress->mSelector == kAudioStreamPropertyPhysicalFormat
                    && fmt != atomic_load(&gStreamFormat[inObjectID]))
                {
                    OSStatus ferr = gHost->RequestDeviceConfigurationChange(gHost, kDevices[dev].device,
                        AVM_ACTION(kAVM_ActionSetFormat, 0, ((UInt32)inObjectID << 8) | (UInt32)fmt), NULL);
                    if (ferr != 0) {
                        os_log_error(gLog, "AVMatrix: el host rechazó el cambio de formato (%d)", (int)ferr);
                        return ferr;
                    }
                }
                return 0;
            }
        }
        return AVM_Stream_HasProperty(inAddress) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareUnknownPropertyError;
    }

    if (inObjectID == kObj_PlugIn)
        return AVM_PlugIn_HasProperty(inAddress) ? kAudioHardwareUnsupportedOperationError : kAudioHardwareUnknownPropertyError;
    return kAudioHardwareBadObjectError;
}

// ---------------------------------------------------------------------------
#pragma mark IO

static OSStatus AVM_StartIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gClockMutex);
    // Durante un cambio de frecuencia no se arranca IO en ningún dispositivo.
    // Tampoco mientras los dispositivos puedan estar a frecuencias distintas.
    avm_rate_status st = avm_rate_io_start(&gRate, (uint32_t)dev, AVM_NowNs());
    if (st != AVM_RATE_OK) {
        pthread_mutex_unlock(&gClockMutex);
        if (st == AVM_RATE_INCONSISTENT)
            os_log_error(gLog, "AVMatrix: audio bloqueado: frecuencias posiblemente distintas; recupere con 'avmctl recover'");
        return kAudioHardwareNotReadyError;
    }
    if (gRate.running_total == 1) {
        // Primer cliente en cualquiera de los tres dispositivos: nueva línea de
        // tiempo y buffers vacíos. Nada de audio de una sesión anterior.
        avm_engine_reset(gEngine);
        AVM_UpdateTicksPerFrame_Locked();
        gAnchorHostTime = (Float64)mach_absolute_time();
    }
    if (gRate.running[dev] == 1) {
        // Este dispositivo empieza (o reanuda) su audio mientras los demás pueden
        // seguir activos: sus canales toman el volumen y el MUTE vigentes desde
        // la primera muestra, sin rampa y sin reiniciar a los demás. (El motor
        // también lo hace solo si el primer bloque no es contiguo al anterior.)
        avm_engine_request_settle_src(gEngine, kDevices[dev].srcGroup);
        if (kDevices[dev].dstGroup >= 0) avm_engine_request_settle_dst(gEngine, (avm_dst_group)kDevices[dev].dstGroup);
    }
    pthread_mutex_unlock(&gClockMutex);
    return 0;
}

static OSStatus AVM_StopIO(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;

    pthread_mutex_lock(&gClockMutex);
    avm_rate_io_stop(&gRate, (uint32_t)dev);
    pthread_mutex_unlock(&gClockMutex);
    return 0;
}

static OSStatus AVM_GetZeroTimeStamp(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, Float64* outSampleTime, UInt64* outHostTime, UInt64* outSeed)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (AVM_DeviceIndex(inDeviceObjectID) < 0) return kAudioHardwareBadObjectError;

    // Los tres dispositivos usan la misma ancla: sus posiciones en frames coinciden,
    // lo que permite pasar audio de uno a otro sin deriva.
    pthread_mutex_lock(&gClockMutex);
    Float64 now = (Float64)mach_absolute_time();
    Float64 ticksPerPeriod = gHostTicksPerFrame * (Float64)kAVM_ZeroTimeStampPeriod;
    Float64 elapsed = now - gAnchorHostTime;
    UInt64 periods = (elapsed > 0 && ticksPerPeriod > 0) ? (UInt64)(elapsed / ticksPerPeriod) : 0;
    *outSampleTime = (Float64)(periods * kAVM_ZeroTimeStampPeriod);
    Float64 host = gAnchorHostTime + (Float64)periods * ticksPerPeriod;
    *outHostTime = host > 0 ? (UInt64)host : 0;
    *outSeed = 1;
    pthread_mutex_unlock(&gClockMutex);
    return 0;
}

static OSStatus AVM_WillDoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, Boolean* outWillDo, Boolean* outWillDoInPlace)
{
    (void)inClientID;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;

    bool will = false;
    if (inOperationID == kAudioServerPlugInIOOperationReadInput)
        will = kDevices[dev].inStream != 0;
    else if (inOperationID == kAudioServerPlugInIOOperationWriteMix)
        will = true;
    if (outWillDo) *outWillDo = will;
    if (outWillDoInPlace) *outWillDoInPlace = true;
    return 0;
}

static OSStatus AVM_BeginIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (AVM_DeviceIndex(inDeviceObjectID) < 0) return kAudioHardwareBadObjectError;
    return 0;
}

static OSStatus AVM_DoIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, AudioObjectID inStreamObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo, void* ioMainBuffer, void* ioSecondaryBuffer)
{
    (void)inClientID; (void)ioSecondaryBuffer;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    int dev = AVM_DeviceIndex(inDeviceObjectID);
    if (dev < 0) return kAudioHardwareBadObjectError;
    bool isInput = false;
    int sdev = AVM_StreamIndex(inStreamObjectID, &isInput);
    if (sdev != dev || ioMainBuffer == NULL || inIOCycleInfo == NULL) return kAudioHardwareBadObjectError;

    const AVM_DeviceDesc* d = &kDevices[dev];
    avm_format fmt = (avm_format)atomic_load(&gStreamFormat[inStreamObjectID]);
    UInt32 channels = AVM_StreamChannels(dev, isInput);
    bool active = atomic_load(&gStreamActive[inStreamObjectID]);

    if (inOperationID == kAudioServerPlugInIOOperationReadInput && isInput && d->dstGroup >= 0) {
        if (!active) {
            memset(ioMainBuffer, 0, (size_t)inIOBufferFrameSize * channels * avm_format_bytes_per_sample(fmt));
            return 0;
        }
        avm_engine_read_metered(gEngine, (avm_dst_group)d->dstGroup, (int64_t)inIOCycleInfo->mInputTime.mSampleTime,
                                inIOBufferFrameSize, ioMainBuffer, fmt, channels, AVM_NowNs());
    } else if (inOperationID == kAudioServerPlugInIOOperationWriteMix && !isInput) {
        if (!active) return 0;
        avm_engine_write_metered(gEngine, d->srcGroup, (int64_t)inIOCycleInfo->mOutputTime.mSampleTime,
                                 inIOBufferFrameSize, ioMainBuffer, fmt, channels, AVM_NowNs());
    }
    return 0;
}

static OSStatus AVM_EndIOOperation(AudioServerPlugInDriverRef inDriver, AudioObjectID inDeviceObjectID, UInt32 inClientID, UInt32 inOperationID, UInt32 inIOBufferFrameSize, const AudioServerPlugInIOCycleInfo* inIOCycleInfo)
{
    (void)inClientID; (void)inOperationID; (void)inIOBufferFrameSize; (void)inIOCycleInfo;
    if (inDriver != gDriverRef) return kAudioHardwareBadObjectError;
    if (AVM_DeviceIndex(inDeviceObjectID) < 0) return kAudioHardwareBadObjectError;
    return 0;
}
