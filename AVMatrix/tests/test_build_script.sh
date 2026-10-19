#!/bin/bash
# Ejecuta scripts/build.sh completo con una cadena de herramientas simulada
# (tests/fakes): comprueba que cada orden recibe sus fuentes y que se ejecutan
# las pruebas del núcleo, de frecuencia y del instalador (esta última una vez).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BUILD_SCRIPT="${AVM_BUILD_SCRIPT:-$ROOT/scripts/build.sh}"
OUT=$(mktemp -d)
LOG="$OUT/build.log"
PASS=0; FAIL=0
check() { local d="$1"; shift; if "$@"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FALLO: $d"; fi; }

# Icono de prueba (firma «icns») y rutas aisladas: la prueba no escribe en el proyecto.
printf 'icns-prueba-build' > "$OUT/test.icns"
export AVM_PROJECT_ICON="$OUT/proyecto/AVMatrix.icns"
export AVM_INSTALLED_APP="$OUT/no-instalada.app"

PATH="$HERE/fakes:$PATH" AVM_BUILD_OUT="$OUT/mac" AVM_ICON_SOURCE="$OUT/test.icns" bash "$BUILD_SCRIPT" > "$LOG" 2>&1
rc=$?
check "build.sh termina bien" [ $rc -eq 0 ]
check "sin 'no input files'" bash -c "! grep -q 'no input files' '$LOG'"
check "se ejecutan las pruebas del núcleo" grep -q "Resultado: [0-9]* comprobaciones correctas, 0 fallos" "$LOG"
check "se ejecutan las pruebas de frecuencia (2 resultados en total)" [ "$(grep -c 'Resultado: [0-9]* comprobaciones correctas, 0 fallos' "$LOG")" -eq 2 ]
check "las pruebas del instalador se ejecutan una sola vez" [ "$(grep -c '^Instalador: ' "$LOG")" -eq 1 ]
check "se generan driver, app y avmctl" [ -f "$OUT/mac/AVMatrix.driver/Contents/MacOS/AVMatrix" ] && [ -f "$OUT/mac/AV Matrix.app/Contents/MacOS/AVMatrix" ] && [ -f "$OUT/mac/avmctl" ]
check "el driver incluye avm_rate.c" grep -q "avm_rate.c" "$OUT/mac/AVMatrix.driver/Contents/MacOS/AVMatrix"
check "la app incluye avm_names.c (nombres de canales)" grep -q "avm_names.c" "$OUT/mac/AV Matrix.app/Contents/MacOS/AVMatrix"
check "se ejecutan las pruebas de nombres" grep -q "^Nombres: [0-9]* comprobaciones correctas, 0 fallos" "$LOG"
check "se compila y ejecuta la prueba nativa del panel de nombres (con la app)" grep -q "^Prueba nativa simulada: tests/test_rename_ui.m .*avm_names.c" "$LOG"
check "se compila y ejecuta la prueba nativa de la pantalla principal y la ayuda" grep -q "^Prueba nativa simulada: tests/test_main_ui.m App/avm_manual.c" "$LOG"
check "la app incluye el manual (avm_manual.c)" grep -q "avm_manual.c" "$OUT/mac/AV Matrix.app/Contents/MacOS/AVMatrix"
check "se ejecutan las pruebas del manual" grep -q "^Manual: [0-9]* comprobaciones correctas, 0 fallos" "$LOG"
APPR="$OUT/mac/AV Matrix.app/Contents"
check "el icono se incluye en la app" cmp -s "$OUT/test.icns" "$APPR/Resources/AVMatrix.icns"
check "Info.plist de la app declara CFBundleIconFile" grep -q "<key>CFBundleIconFile</key>" "$APPR/Info.plist"
if [ $FAIL -ne 0 ]; then echo "--- registro de build.sh ---"; tail -20 "$LOG"; fi

# Sin icono en ninguna parte: build.sh se detiene (no genera una app con el icono genérico).
PATH="$HERE/fakes:$PATH" AVM_BUILD_OUT="$OUT/mac2" bash "$BUILD_SCRIPT" > "$OUT/b2.log" 2>&1
rc=$?
check "sin icono: build.sh devuelve error" [ $rc -ne 0 ]
check "sin icono: explica qué falta" grep -q "no se encontró el icono AVMatrix.icns" "$OUT/b2.log"

# Con la app instalada que ya tiene el icono: se importa al proyecto y se incluye.
mkdir -p "$OUT/instalada.app/Contents/Resources"
printf '<plist><dict><key>CFBundleIconFile</key>\n<string>Personal</string></dict></plist>\n' > "$OUT/instalada.app/Contents/Info.plist"
printf 'icns-de-antonio' > "$OUT/instalada.app/Contents/Resources/Personal.icns"
PATH="$HERE/fakes:$PATH" AVM_BUILD_OUT="$OUT/mac3" AVM_INSTALLED_APP="$OUT/instalada.app" bash "$BUILD_SCRIPT" > "$OUT/b3.log" 2>&1
rc=$?
check "icono instalado: build.sh termina bien" [ $rc -eq 0 ]
check "icono instalado: se importa al proyecto" cmp -s "$OUT/instalada.app/Contents/Resources/Personal.icns" "$AVM_PROJECT_ICON"
check "icono instalado: queda dentro de la nueva app" cmp -s "$AVM_PROJECT_ICON" "$OUT/mac3/AV Matrix.app/Contents/Resources/AVMatrix.icns"
check "icono instalado: lo indica" grep -q "Icono importado de la app instalada" "$OUT/b3.log"

# Un archivo que no es .icns se rechaza.
printf 'PNG' > "$OUT/malo.icns"
PATH="$HERE/fakes:$PATH" AVM_BUILD_OUT="$OUT/mac4" AVM_ICON_SOURCE="$OUT/malo.icns" bash "$BUILD_SCRIPT" > "$OUT/b4.log" 2>&1
rc=$?
check "icono inválido: build.sh devuelve error" [ $rc -ne 0 ]
check "icono inválido: lo explica" grep -q "no es un archivo .icns válido" "$OUT/b4.log"
rm -rf "$OUT"
echo "build.sh: $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
