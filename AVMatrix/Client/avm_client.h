// AV Matrix — funciones de cliente CoreAudio compartidas por avmctl y la app.

#ifndef AVM_CLIENT_H
#define AVM_CLIENT_H

#include <CoreAudio/CoreAudio.h>
#include <stdbool.h>

#include "../Driver/src/avm_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AVM_UID_SMAART    "AVMatrix_Smaart_UID"
#define AVM_UID_LISTENTO  "AVMatrix_Listento_UID"
#define AVM_UID_MUSIC     "AVMatrix_Music_UID"
#define AVM_UID_DANTE     "AVMatrix_Dante_UID"   // puente: 2 entradas (Dante L/R) que la app envía a DVS
#define AVM_UID_EVO       "AVMatrix_EVO_UID"     // puente: 4 salidas (EVO 1-4) que la app recibe de la EVO
#define AVM_UID_PREFIX    "AVMatrix_"
#define AVM_PROPERTY_MATRIX 'mtrx'
#define AVM_PROPERTY_RATE_STATE 'rsta'
#define AVM_PROPERTY_METERS 'metr'
#define AVM_PROPERTY_MIXER 'mixr'

// Estado de la frecuencia según el driver.
typedef enum { AVM_RATE_HEALTH_OK = 0, AVM_RATE_HEALTH_CHANGING = 1, AVM_RATE_HEALTH_INCONSISTENT = 2 } avm_rate_health_t;

typedef enum { AVM_DEV_SMAART = 0, AVM_DEV_LISTENTO = 1, AVM_DEV_MUSIC = 2,
               AVM_DEV_DANTE = 3, AVM_DEV_EVO = 4, AVM_DEV_COUNT = 5 } avm_device_kind;

// Busca un dispositivo de AV Matrix por tipo. kAudioObjectUnknown si no está instalado.
AudioObjectID avm_find_device(avm_device_kind kind);
const char*   avm_device_uid(avm_device_kind kind);
const char*   avm_device_label(avm_device_kind kind);

OSStatus avm_get_matrix(avm_matrix* out);
OSStatus avm_set_matrix(const avm_matrix* m);

OSStatus avm_get_rate_health(int* out);
// Vuelve a aplicar una frecuencia en los tres dispositivos (0 = la actual).
// Repara el estado inconsistente; requiere que no haya audio activo.
OSStatus avm_recover_rate(Float64 rate);

// Lee los 34 medidores del driver (18 fuentes antes de la matriz, 16 destinos
// después). Solo consulta una propiedad: no abre audio ni lo modifica.
OSStatus avm_get_meters(avm_meter_reading src[AVM_NUM_SOURCES], avm_meter_reading dst[AVM_NUM_DESTS]);

// Mixer: volumen (solo atenuación) y mute de las 18 fuentes y los 16 destinos.
// El driver rechaza ajustes no válidos (ganancia fuera de [0, 1]) y los guarda.
OSStatus avm_get_mixer(avm_mixer* out);
OSStatus avm_set_mixer(const avm_mixer* mx);

OSStatus avm_get_sample_rate(Float64* out);
OSStatus avm_set_sample_rate(Float64 rate);

// Formato físico de las entradas (input=true) o salidas de un dispositivo.
OSStatus avm_get_physical_format(avm_device_kind kind, bool input, AudioStreamBasicDescription* out);
OSStatus avm_set_physical_format(avm_device_kind kind, bool input, avm_format fmt);
const char* avm_format_name(avm_format fmt);
int avm_format_from_asbd(const AudioStreamBasicDescription* d);

// --- Dispositivos de audio del Mac (para los puentes Dante y EVO) -------------
typedef struct {
    AudioObjectID id;
    char          uid[256];
    char          name[256];
    UInt32        in_channels;     // canales de entrada reales que expone
    UInt32        out_channels;
    Float64       rate;            // frecuencia nominal actual
    bool          alive;
    bool          is_avmatrix;     // uno de los dispositivos de AV Matrix (nunca se usa como externo)
} avm_hw_device;

// Lista los dispositivos de audio (como mucho `max`). Devuelve cuántos hay.
int      avm_list_hw_devices(avm_hw_device* out, int max);
// Busca un dispositivo por UID. kAudioHardwareBadDeviceError si no está.
OSStatus avm_hw_device_by_uid(const char* uid, avm_hw_device* out);
OSStatus avm_hw_device_info(AudioObjectID id, avm_hw_device* out);
// Cambia la frecuencia nominal de un dispositivo externo (EVO o DVS).
OSStatus avm_hw_set_rate(AudioObjectID id, Float64 rate);

// Nombres legibles de fuentes y destinos de la matriz.
void avm_source_name(int src, char* buf, size_t len);
void avm_dest_name(int dst, char* buf, size_t len);
// Nombre corto para la línea de comandos: smaart.out.3, listento.out.1, music.L, smaart.in.2, listento.in.8
void avm_source_token(int src, char* buf, size_t len);
void avm_dest_token(int dst, char* buf, size_t len);
int  avm_parse_source(const char* token);
int  avm_parse_dest(const char* token);

#ifdef __cplusplus
}
#endif

#endif
