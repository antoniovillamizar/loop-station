#!/bin/bash
# Busca el icono personalizado de AV Matrix y lo copia a la app indicada.
#
#   scripts/resolve_icon.sh "<ruta>/AV Matrix.app"
#
# Orden de búsqueda:
#   1. AVM_ICON_SOURCE (si está definida)
#   2. App/Resources/AVMatrix.icns del proyecto
#   3. el icono de la app ya instalada (AVM_INSTALLED_APP, por defecto
#      "/Applications/AV Matrix.app"), que se importa a App/Resources/AVMatrix.icns
#      para que las compilaciones siguientes lo incluyan siempre.
# Comprueba que sea un .icns real (firma «icns») y que Info.plist lo declare.
# Códigos: 0 = icono copiado; 3 = no se encontró icono; 1 = error.
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
APP_BUNDLE="${1:?Uso: resolve_icon.sh <app>}"
PROJECT_ICON="${AVM_PROJECT_ICON:-$ROOT/App/Resources/AVMatrix.icns}"  # configurable solo para pruebas
INSTALLED="${AVM_INSTALLED_APP:-/Applications/AV Matrix.app}"

is_icns() { [ -f "$1" ] && [ "$(head -c 4 "$1" 2>/dev/null)" = "icns" ]; }

# Icono declarado por una app instalada (CFBundleIconFile, con o sin .icns).
installed_icon() {
    local plist="$INSTALLED/Contents/Info.plist" name=""
    if [ -f "$plist" ]; then
        name=$(grep -A1 "<key>CFBundleIconFile</key>" "$plist" 2>/dev/null | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p' | head -1 || true)
    fi
    for cand in "${name:+$INSTALLED/Contents/Resources/$name}" "${name:+$INSTALLED/Contents/Resources/$name.icns}" "$INSTALLED/Contents/Resources/AVMatrix.icns" "$INSTALLED"/Contents/Resources/*.icns; do
        [ -n "$cand" ] && is_icns "$cand" && { echo "$cand"; return 0; }
    done
    return 1
}

SRC=""
if [ -n "${AVM_ICON_SOURCE:-}" ]; then
    is_icns "$AVM_ICON_SOURCE" || { echo "ERROR: AVM_ICON_SOURCE no es un archivo .icns válido: $AVM_ICON_SOURCE" >&2; exit 1; }
    SRC="$AVM_ICON_SOURCE"
elif [ -f "$PROJECT_ICON" ]; then
    is_icns "$PROJECT_ICON" || { echo "ERROR: $PROJECT_ICON no es un archivo .icns válido" >&2; exit 1; }
    SRC="$PROJECT_ICON"
elif FOUND=$(installed_icon); then
    mkdir -p "$(dirname "$PROJECT_ICON")"
    cp -p "$FOUND" "$PROJECT_ICON"
    echo "Icono importado de la app instalada: $FOUND -> App/Resources/AVMatrix.icns"
    SRC="$PROJECT_ICON"
else
    exit 3
fi

PLIST="$APP_BUNDLE/Contents/Info.plist"
grep -q "<key>CFBundleIconFile</key>" "$PLIST" || { echo "ERROR: $PLIST no declara CFBundleIconFile" >&2; exit 1; }
mkdir -p "$APP_BUNDLE/Contents/Resources"
cp "$SRC" "$APP_BUNDLE/Contents/Resources/AVMatrix.icns"
echo "Icono: $SRC"
