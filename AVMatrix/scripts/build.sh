#!/bin/bash
# Compila AV Matrix en el Mac: driver HAL, app de matriz y avmctl.
# Requiere las Command Line Tools de Xcode (xcode-select --install).
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
OUT="${AVM_BUILD_OUT:-$ROOT/build/mac}"
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
    Driver/src/AVMatrixDriver.c Driver/src/avm_core.c Driver/src/avm_rate.c \
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
    App/AVMatrixApp.m Client/avm_client.c Client/avm_names.c Driver/src/avm_core.c \
    -framework Cocoa -framework CoreAudio -framework CoreFoundation
# Icono personalizado (antes de firmar: queda sellado en la firma del bundle).
set +e
bash scripts/resolve_icon.sh "$APP"
icon_rc=$?
set -e
if [ $icon_rc -eq 3 ]; then
    if [ -n "${AVM_ALLOW_GENERIC_ICON:-}" ]; then
        echo "Aviso: sin AVMatrix.icns; la app tendrá el icono genérico (AVM_ALLOW_GENERIC_ICON)."
    else
        echo "ERROR: no se encontró el icono AVMatrix.icns." >&2
        echo "Cópielo a App/Resources/AVMatrix.icns (o defina AVM_ICON_SOURCE=<archivo>) y vuelva a compilar." >&2
        exit 1
    fi
elif [ $icon_rc -ne 0 ]; then
    exit 1
fi
codesign --force --sign "${AVM_SIGN_IDENTITY:--}" --timestamp=none "$APP"

echo "== Pruebas (núcleo, frecuencia, instalador) =="
clang $CFLAGS -o "$OUT/test_core" tests/test_core.c Driver/src/avm_core.c
"$OUT/test_core"
clang $CFLAGS -o "$OUT/test_rate" tests/test_rate.c Driver/src/avm_rate.c
"$OUT/test_rate"
clang $CFLAGS -o "$OUT/test_meters" tests/test_meters.c Driver/src/avm_core.c
"$OUT/test_meters"
clang $CFLAGS -o "$OUT/test_names" tests/test_names.c Client/avm_names.c
"$OUT/test_names"
bash tests/test_install.sh
bash tests/test_app_strings.sh
# Prueba nativa del panel de nombres (AppKit): tamaño, mensajes de error y
# liberación de memoria. Muestra brevemente el panel en pantalla; con
# AVM_UI_NO_WINDOW=1 solo hace las comprobaciones sin ventana.
clang -fobjc-arc -O2 -Wall -Wextra -Werror -Wno-four-char-constants -mmacosx-version-min=$MINOS \
    -o "$OUT/test_rename_ui" tests/test_rename_ui.m Client/avm_client.c Client/avm_names.c Driver/src/avm_core.c \
    -framework Cocoa -framework CoreAudio -framework CoreFoundation
"$OUT/test_rename_ui"

echo "== Verificación =="
lipo -info "$DRV/Contents/MacOS/AVMatrix"
codesign --verify --strict --deep --verbose=2 "$DRV"
codesign --verify --strict --deep --verbose=2 "$APP"
codesign --verify --strict --verbose=2 "$OUT/avmctl"
plutil -lint "$DRV/Contents/Info.plist" "$APP/Contents/Info.plist"
if [ -f "$APP/Contents/Resources/AVMatrix.icns" ]; then echo "Icono incluido: $APP/Contents/Resources/AVMatrix.icns"; fi
codesign -dvv "$DRV" 2>&1 | grep -E "^(Signature|Authority|TeamIdentifier)=" || true
echo
echo "Listo: $OUT"
echo "Instalar: sudo scripts/install.sh"
