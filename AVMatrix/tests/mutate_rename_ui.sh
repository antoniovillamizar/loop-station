#!/bin/bash
# Mutaciones de la prueba nativa del panel de nombres (solo macOS).
# Compila tests/test_rename_ui.m contra copias alteradas de la app y comprueba
# que la prueba FALLA con el motivo esperado:
#   M1  el controlador vuelve a retener su popover (ciclo real) -> «siguen vivos»
#   M2  sin el ancho fijo de las filas (el texto ensancha el panel) -> «título largo»
# No modifica App/AVMatrixApp.m: las copias se crean junto a ella y se borran.
set -uo pipefail
cd "$(dirname "$0")/.."
APP=App/AVMatrixApp.m
OUT=$(mktemp -d)
PASS=0; FAIL=0
cleanup() { rm -f App/.mut_*.m; rm -rf "$OUT"; }
trap cleanup EXIT

mutate() {   # mutate <nombre> <motivo esperado> <expresión perl>
    local name="$1" expect="$2" expr="$3" m="App/.mut_$1.m"
    perl -0pe "$expr" "$APP" > "$m"
    if cmp -s "$APP" "$m"; then echo "FALLO: $name no cambió nada (patrón obsoleto)"; FAIL=$((FAIL+1)); return; fi
    if ! clang -fobjc-arc -O0 -Wno-four-char-constants -DAVM_APP_FILE="\"../$m\"" -o "$OUT/t_$name" \
            tests/test_rename_ui.m App/avm_manual.c App/avm_fader.c Client/avm_client.c Client/avm_names.c Driver/src/avm_core.c \
            -framework Cocoa -framework CoreAudio -framework CoreFoundation 2> "$OUT/$name.cc"; then
        echo "FALLO: $name no compila"; cat "$OUT/$name.cc"; FAIL=$((FAIL+1)); return
    fi
    AVM_UI_NO_WINDOW=1 "$OUT/t_$name" > "$OUT/$name.log" 2>&1
    local rc=$?
    if [ $rc -ne 0 ] && grep -q "FALLO: .*$expect" "$OUT/$name.log"; then
        echo "$name detectada: $(grep -m1 "FALLO: .*$expect" "$OUT/$name.log")"; PASS=$((PASS+1))
    else
        echo "FALLO: $name NO detectada (código $rc)"; cat "$OUT/$name.log"; FAIL=$((FAIL+1))
    fi
}

mutate M1 "siguen vivos" 's/\@property \(weak\) NSPopover\* popover;/\@property (strong) NSPopover* popover;/'
mutate M2 "título largo" 's/\n\s*\[row\.widthAnchor constraintEqualToConstant:inner\]\.active = YES;//'

echo "Mutaciones del panel: $PASS detectadas, $FAIL fallos"
[ $FAIL -eq 0 ]
