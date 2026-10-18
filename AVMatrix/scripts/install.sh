#!/bin/bash
# Instala AV Matrix. Uso: sudo scripts/install.sh
# Antes de reemplazar cualquier archivo existente hace una copia de seguridad.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Ejecute con sudo: sudo $0" >&2
    exit 1
fi

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
if [ -d "$ROOT/build/mac/AVMatrix.driver" ]; then
    SRC="$ROOT/build/mac"
elif [ -d "$ROOT/dist/AVMatrix.driver" ]; then
    SRC="$ROOT/dist"
    echo "Aviso: se instala la compilación precompilada de dist/. Se recomienda scripts/build.sh."
else
    echo "No hay compilación. Ejecute primero scripts/build.sh" >&2
    exit 1
fi

HAL="/Library/Audio/Plug-Ins/HAL"
APPS="/Applications"
BIN="/usr/local/bin"
USER_HOME=$(eval echo "~${SUDO_USER:-root}")
STAMP=$(date +%Y%m%d_%H%M%S)
BACKUP="$USER_HOME/AVMatrix-backups/$STAMP"

backup() {
    if [ -e "$1" ]; then
        mkdir -p "$BACKUP"
        cp -Rp "$1" "$BACKUP/"
        echo "Copia de seguridad: $1 -> $BACKUP/"
    fi
}

backup "$HAL/AVMatrix.driver"
backup "$APPS/AV Matrix.app"
backup "$BIN/avmctl"
if [ -d "$BACKUP" ] && [ -n "${SUDO_USER:-}" ]; then chown -R "$SUDO_USER" "$USER_HOME/AVMatrix-backups"; fi

mkdir -p "$HAL" "$BIN"
rm -rf "$HAL/AVMatrix.driver"
cp -R "$SRC/AVMatrix.driver" "$HAL/"
chown -R root:wheel "$HAL/AVMatrix.driver"
chmod -R go-w "$HAL/AVMatrix.driver"
xattr -dr com.apple.quarantine "$HAL/AVMatrix.driver" 2>/dev/null || true
# Firma ad hoc en esta máquina (necesaria en Apple Silicon si el binario viene de otra máquina).
codesign --force --sign - "$HAL/AVMatrix.driver" >/dev/null 2>&1 || true

rm -rf "$APPS/AV Matrix.app"
cp -R "$SRC/AV Matrix.app" "$APPS/"
xattr -dr com.apple.quarantine "$APPS/AV Matrix.app" 2>/dev/null || true
codesign --force --sign - "$APPS/AV Matrix.app" >/dev/null 2>&1 || true

cp "$SRC/avmctl" "$BIN/avmctl"
chmod 755 "$BIN/avmctl"
xattr -d com.apple.quarantine "$BIN/avmctl" 2>/dev/null || true
codesign --force --sign - "$BIN/avmctl" >/dev/null 2>&1 || true

echo "Reiniciando el servicio de audio (coreaudiod). El audio se cortará unos segundos."
killall coreaudiod 2>/dev/null || launchctl kickstart -k system/com.apple.audio.coreaudiod || true
sleep 3

echo
/usr/local/bin/avmctl status || {
    echo
    echo "Los dispositivos aún no aparecen. Espere unos segundos y ejecute: avmctl status"
    echo "Diagnóstico: log show --last 2m --predicate 'process CONTAINS \"coreaudiod\" OR process CONTAINS \"Core Audio Driver\"' | grep -i avmatrix"
}
