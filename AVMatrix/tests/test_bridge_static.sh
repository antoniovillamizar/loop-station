#!/bin/bash
# Comprobaciones estáticas de los puentes Dante y EVO (la parte de CoreAudio y
# de la app no se puede ejecutar fuera de macOS).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BR="${AVM_BRIDGE_SOURCE:-$HERE/../Client/avm_bridge.c}"
APP="${AVM_APP_SOURCE:-$HERE/../App/AVMatrixApp.m}"
PLIST="$HERE/../App/Info.plist"
DRV="$HERE/../Driver/src/AVMatrixDriver.c"
PASS=0; FAIL=0
check() { local d="$1"; shift; if "$@"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FALLO: $d"; fi; }
B=$(sed 's://.*$::' "$BR"); A=$(sed 's://.*$::' "$APP")

# Puente (CoreAudio)
check "agregado privado (solo la app lo ve)" bash -c 'grep -q "dict_set_int(desc, kAudioAggregateDeviceIsPrivateKey, 1);" <<< "$1"' _ "$B"
check "reloj del agregado = dispositivo externo (DVS o EVO)" bash -c 'grep -q "kAudioAggregateDeviceClockDeviceKey, ext->uid" <<< "$1" && grep -q "kAudioAggregateDeviceMainSubDeviceKey, ext->uid" <<< "$1"' _ "$B"
check "compensación de deriva en el lado de AV Matrix" bash -c 'grep -q "dict_set_int(sa, kAudioSubDeviceDriftCompensationKey, 1);" <<< "$1"' _ "$B"
check "AV Matrix primero en el agregado" bash -c 'grep -n "CFArrayAppendValue(subs, sa);" <<< "$1" | head -1 | grep -q . && [ $(grep -n "CFArrayAppendValue(subs, sa);" <<< "$1" | cut -d: -f1) -lt $(grep -n "CFArrayAppendValue(subs, se);" <<< "$1" | cut -d: -f1) ]' _ "$B"
check "se rechazan dispositivos de AV Matrix y agregados que los contienen" bash -c 'grep -q "ext.is_avmatrix || contains_avmatrix(ext.id)" <<< "$1"' _ "$B"
check "el IOProc no reserva memoria" bash -c '! sed -n "/^static OSStatus bridge_io/,/^}/p" <<< "$1" | grep -qE "malloc|calloc|CFRelease|printf|dispatch_"' _ "$B"
check "el callback valida todos los buffers con avm_bridge_process" bash -c 'sed -n "/^static OSStatus bridge_io/,/^}/p" <<< "$1" | grep -q "avm_bridge_process(&b->plan, (const avm_abl\*)in, (avm_abl\*)out);"' _ "$B"
check "avm_abl tiene la disposición de AudioBufferList (comprobado al compilar)" bash -c 'grep -q "_Static_assert(offsetof(avm_abl, mBuffers) == offsetof(AudioBufferList, mBuffers)" <<< "$1"' _ "$B"
check "avisos de cambios de streams y canales del dispositivo externo" bash -c 'grep -q "kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeInput" <<< "$1" && grep -q "kAudioDevicePropertyStreams" <<< "$1"' _ "$B"
check "con el puente en marcha se comprueba si hay que reconstruirlo" bash -c 'grep -q "} else if (b->running \&\& !bridge_changed(b, \&ext)) {" <<< "$1"' _ "$B"
check "la reconstrucción detiene el IOProc antes del plan nuevo" bash -c 'grep -q "if (rebuild) bridge_stop(b);" <<< "$1"' _ "$B"
check "la app comprueba los avisos del hilo de IO (poll)" bash -c 'grep -q "avm_bridge_poll(self.danteRow.bridge)" <<< "$1"' _ "$A"
check "cada arranque crea un agregado nuevo (sin audio viejo)" bash -c 'grep -q "AudioHardwareDestroyAggregateDevice(b->agg);" <<< "$1" && grep -q "if (b->running && (!same_route || !c->enabled)) bridge_stop(b);" <<< "$1"' _ "$B"
check "avisos de dispositivos, frecuencia y desconexión" bash -c 'grep -q "kAudioHardwarePropertyDevices" <<< "$1" && grep -q "kAudioDevicePropertyDeviceIsAlive" <<< "$1" && grep -q "kAudioDevicePropertyNominalSampleRate" <<< "$1"' _ "$B"

# App
check "permiso de micrófono declarado en Info.plist" grep -q "<key>NSMicrophoneUsageDescription</key>" "$PLIST"
check "la app pide el permiso de micrófono antes de activar un puente" bash -c 'grep -q "requestAccessForMediaType:AVMediaTypeAudio" <<< "$1"' _ "$A"
check "los dispositivos de AV Matrix no se listan como destino del puente" bash -c 'grep -q "if (d->is_avmatrix || n == 0" <<< "$1"' _ "$A"
check "los presets de medición conservan las rutas hacia Dante" bash -c 'grep -q "avm_matrix_copy_dst_group(m, &cur, AVM_DST_GROUP_DANTE)" <<< "$1"' _ "$A"
check "con un puente activo la app sigue al cerrar la ventana" bash -c 'grep -q "return !\[self anyBridgeEnabled\];" <<< "$1"' _ "$A"
check "los puentes se pausan al cambiar la frecuencia" bash -c 'grep -q "\[self setBridgesSuspended:YES\];" <<< "$1"' _ "$A"
check "la configuración de los puentes se guarda" bash -c 'grep -q "BridgeConfigSave(&c);" <<< "$1"' _ "$A"
check "grupos EVO y Dante en la matriz" bash -c 'grep -q "{ \"EVO\",      AVM_SRC_EVO_BASE,    4 }" <<< "$1" && grep -q "{ \"Dante\",    AVM_DST_DANTE_BASE,  2 }" <<< "$1"' _ "$A"

# Driver
check "los dispositivos puente no pueden ser dispositivos por defecto" bash -c 'grep -A3 "\"AV Matrix Dante (puente)\"" "$1" | grep -q "2, 0, -1, AVM_DST_GROUP_DANTE, false" && grep -A3 "\"AV Matrix EVO (puente)\"" "$1" | grep -q "0, 4, AVM_SRC_GROUP_EVO, -1, false"' _ "$DRV"
check "el driver lee la matriz guardada por la versión anterior" grep -q "avm_matrix_from_bytes(CFDataGetBytePtr" "$DRV"

echo "Puentes (estático): $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
