#!/bin/bash
# Comprobaciones estáticas de avmctl (no se puede ejecutar fuera de macOS):
# la autoprueba registra cada fallo al restaurar y termina con su veredicto.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="${AVM_AVMCTL_SOURCE:-$HERE/../Client/avmctl.c}"
PASS=0; FAIL=0
check() { local d="$1"; shift; if "$@"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FALLO: $d"; fi; }
CODE=$(sed 's://.*$::' "$SRC")
check "el fallo al restaurar el mixer se registra" bash -c 'grep -q "if (merr) { print_err(\"restaurar mixer\", merr); out.mixer_restored = false; }" <<< "$1"' _ "$CODE"
check "el fallo al restaurar la matriz se registra" bash -c 'grep -q "out.matrix_restored = false;" <<< "$1"' _ "$CODE"
check "el fallo al restaurar la frecuencia se registra" bash -c 'grep -q "out.rate_restored = false;" <<< "$1"' _ "$CODE"
check "el fallo al restaurar un formato se registra" bash -c 'grep -q "out.formats_restored = false;" <<< "$1"' _ "$CODE"
check "el código de salida sale del veredicto" bash -c 'grep -q "return avm_selftest_exit_code(&out);" <<< "$1"' _ "$CODE"
check "el texto RESULTADO sale del veredicto" bash -c 'grep -q "printf(\"RESULTADO: %s\\\\n\", avm_selftest_verdict(&out));" <<< "$1"' _ "$CODE"
check "ya no se devuelve total_pass sin mirar la restauración" bash -c '! grep -q "return total_pass ? 0 : 1;" <<< "$1"' _ "$CODE"
check "la autoprueba guarda el mixer antes de empezar" bash -c 'grep -q "avm_get_mixer(&saved_mixer)" <<< "$1"' _ "$CODE"
echo "avmctl: $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
