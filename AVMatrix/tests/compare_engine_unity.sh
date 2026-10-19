#!/bin/bash
# Comprueba que, con el mixer a 0 dB y sin mute (estado de una instalación
# nueva), el motor actual produce EXACTAMENTE la misma salida que el motor de
# una revisión anterior (sin mixer): audio de los 16 destinos y medidores, bit a
# bit, con matrices y entradas aleatorias en Float32, SInt32, SInt24 y SInt16.
#   bash tests/compare_engine_unity.sh [revisión]   (por defecto fddbc62)
set -euo pipefail
cd "$(dirname "$0")/.."
BASE="${1:-fddbc62}"
if ! git rev-parse --git-dir >/dev/null 2>&1 || ! git cat-file -e "$BASE^{commit}" 2>/dev/null; then
    echo "(comparación con $BASE omitida: no hay historial git con esa revisión)"
    exit 0
fi
TOP=$(git rev-parse --show-prefix)   # p. ej. "AVMatrix/"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/old" "$T/new"
git show "$BASE:${TOP}Driver/src/avm_core.c" > "$T/old/avm_core.c"
git show "$BASE:${TOP}Driver/src/avm_core.h" > "$T/old/avm_core.h"
cp Driver/src/avm_core.c Driver/src/avm_core.h "$T/new/"
CC="${CC:-cc}"
"$CC" -std=c11 -O2 -I "$T/old" -o "$T/d_old" tests/engine_dump.c "$T/old/avm_core.c" -lm
"$CC" -std=c11 -O2 -I "$T/new" -o "$T/d_new" tests/engine_dump.c "$T/new/avm_core.c" -lm
"$T/d_old" > "$T/old.bin"
"$T/d_new" > "$T/new.bin"
if cmp -s "$T/old.bin" "$T/new.bin"; then
    echo "Motor a 0 dB: idéntico bit a bit a $BASE ($(wc -c < "$T/new.bin" | tr -d ' ') bytes de audio y medidores)"
else
    echo "FALLO: la salida a 0 dB difiere de $BASE"; cmp "$T/old.bin" "$T/new.bin" | head -1; exit 1
fi
