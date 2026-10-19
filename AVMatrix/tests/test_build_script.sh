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

PATH="$HERE/fakes:$PATH" AVM_BUILD_OUT="$OUT/mac" bash "$BUILD_SCRIPT" > "$LOG" 2>&1
rc=$?
check "build.sh termina bien" [ $rc -eq 0 ]
check "sin 'no input files'" bash -c "! grep -q 'no input files' '$LOG'"
check "se ejecutan las pruebas del núcleo" grep -q "Resultado: [0-9]* comprobaciones correctas, 0 fallos" "$LOG"
check "se ejecutan las pruebas de frecuencia (2 resultados en total)" [ "$(grep -c 'Resultado: [0-9]* comprobaciones correctas, 0 fallos' "$LOG")" -eq 2 ]
check "las pruebas del instalador se ejecutan una sola vez" [ "$(grep -c '^Instalador: ' "$LOG")" -eq 1 ]
check "se generan driver, app y avmctl" [ -f "$OUT/mac/AVMatrix.driver/Contents/MacOS/AVMatrix" ] && [ -f "$OUT/mac/AV Matrix.app/Contents/MacOS/AVMatrix" ] && [ -f "$OUT/mac/avmctl" ]
check "el driver incluye avm_rate.c" grep -q "avm_rate.c" "$OUT/mac/AVMatrix.driver/Contents/MacOS/AVMatrix"
if [ $FAIL -ne 0 ]; then echo "--- registro de build.sh ---"; tail -20 "$LOG"; fi
rm -rf "$OUT"
echo "build.sh: $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
