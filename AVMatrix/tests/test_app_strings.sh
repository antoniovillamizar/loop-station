#!/bin/bash
# Comprueba que la app no formatea textos C con %s (macOS los interpreta con la
# codificación del sistema y «Música» sale como «M√∫sica») y que los nombres C
# de grupos y dispositivos pasan por UTF8() / stringWithUTF8String.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${AVM_APP_SOURCE:-$HERE/../App/AVMatrixApp.m}"
PASS=0; FAIL=0
check() { local d="$1"; shift; if "$@"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FALLO: $d"; fi; }

# Código sin comentarios de línea.
CODE=$(sed 's://.*$::' "$APP")

check "ningún %s en cadenas de formato de la app" bash -c '! grep -nE "@\"[^\"]*%s" <<< "$1"' _ "$CODE"
check "los nombres de grupo se convierten con UTF8()" bash -c '! grep -nE "k(Tx|Rx)Groups\[[^]]*\]\.name" <<< "$1" | grep -v "UTF8(k" | grep -q .' _ "$CODE"
check "las etiquetas de dispositivo se convierten con UTF8()" bash -c '! grep -n "avm_device_label(" <<< "$1" | grep -v "UTF8(avm_device_label" | grep -q .' _ "$CODE"
check "UTF8() usa stringWithUTF8String" grep -q "stringWithUTF8String:s" "$APP"
for f in "$APP" "$HERE/../Client/avm_client.c" "$HERE/../Client/avmctl.c"; do
    check "$(basename "$f") es UTF-8 válido" bash -c 'iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1' _ "$f"
done
check "«Música» está escrito en UTF-8 en los nombres de grupo" grep -q '{ "Música",' "$APP"
check "los alias se convierten con UTF8()" grep -q "return a ? UTF8(a) : nil;" "$APP"
check "los alias se guardan en forma compuesta NFC (tildes)" grep -q "precomposedStringWithCanonicalMapping" "$APP"
check "la ruta del archivo de nombres se pasa en UTF-8" grep -q "NamesFilePath().fileSystemRepresentation" "$APP"
check "un alias nunca se usa como cadena de formato" bash -c '! grep -nE "stringWithFormat:(ChannelAlias|RxChannelName|TxChannelName|a)[,)]" <<< "$1" | grep -q .' _ "$CODE"

echo "Textos de la app: $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
