#!/bin/bash
# Compila AV Matrix en el Mac: driver HAL, app de matriz y avmctl.
# Requiere las Command Line Tools de Xcode (xcode-select --install).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="$ROOT/build/mac"
MINOS=12.0
ARCHS="-arch arm64 -arch x86_64"
CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror -Wno-four-char-constants -mmacosx-version-min=$MINOS"

echo "== Sistema =="
sw_vers || true
uname -m
echo "== Herramientas =="
if ! xcode-select -p >/dev/null 2>&1; then
    echo "Faltan las Command Line Tools. Ejecute: xcode-select --install" >&2
    exit 1
fi
xcode-select -p
clang --version | head -1
SDK="$(xcrun --sdk macosx --show-sdk-path)"
echo "SDK: $SDK"

rm -rf "$OUT"
mkdir -p "$OUT"

echo "== Driver =="
DRV="$OUT/AVMatrix.driver"
mkdir -p "$DRV/Contents/MacOS"
cp Driver/Info.plist "$DRV/Contents/Info.plist"
clang $ARCHS $CFLAGS -fvisibility=hidden -bundle \
    -o "$DRV/Contents/MacOS/AVMatrix" \
    Driver/src/AVMatrixDriver.c Driver/src/avm_core.c \
    -framework CoreFoundation -framework CoreAudio
codesign --force --sign "${AVM_SIGN_IDENTITY:--}" --timestamp=none "$DRV"

echo "== avmctl =="
clang $ARCHS $CFLAGS -o "$OUT/avmctl" \
    Client/avmctl.c Client/avm_client.c Driver/src/avm_core.c \
    -framework CoreAudio -framework CoreFoundation
codesign --force --sign "${AVM_SIGN_IDENTITY:--}" --timestamp=none "$OUT/avmctl"

echo "== App =="
APP="$OUT/AV Matrix.app"
mkdir -p "$APP/Contents/MacOS"
cp App/Info.plist "$APP/Contents/Info.plist"
clang $ARCHS -fobjc-arc -O2 -Wall -Wextra -Werror -Wno-four-char-constants -mmacosx-version-min=$MINOS \
    -o "$APP/Contents/MacOS/AVMatrix" \
    App/AVMatrixApp.m Client/avm_client.c Driver/src/avm_core.c \
    -framework Cocoa -framework CoreAudio -framework CoreFoundation
codesign --force --sign "${AVM_SIGN_IDENTITY:--}" --timestamp=none "$APP"

echo "== Pruebas del núcleo =="
clang $CFLAGS -o "$OUT/test_core" tests/test_core.c Driver/src/avm_core.c
"$OUT/test_core"

echo "== Verificación =="
lipo -info "$DRV/Contents/MacOS/AVMatrix"
codesign --verify --verbose=2 "$DRV"
plutil -lint "$DRV/Contents/Info.plist"
echo
echo "Listo: $OUT"
echo "Instalar: sudo scripts/install.sh"
