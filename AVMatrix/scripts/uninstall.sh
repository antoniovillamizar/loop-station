#!/bin/bash
# Desinstala AV Matrix. Uso: sudo scripts/uninstall.sh
# Guarda una copia de seguridad de lo que elimina.
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "Ejecute con sudo: sudo $0" >&2
    exit 1
fi

HAL="/Library/Audio/Plug-Ins/HAL/AVMatrix.driver"
APP="/Applications/AV Matrix.app"
BIN="/usr/local/bin/avmctl"
USER_HOME=$(eval echo "~${SUDO_USER:-root}")
BACKUP="$USER_HOME/AVMatrix-backups/uninstall_$(date +%Y%m%d_%H%M%S)"

for f in "$HAL" "$APP" "$BIN"; do
    if [ -e "$f" ]; then
        mkdir -p "$BACKUP"
        cp -Rp "$f" "$BACKUP/"
        rm -rf "$f"
        echo "Eliminado: $f (copia en $BACKUP)"
    fi
done
if [ -d "$BACKUP" ] && [ -n "${SUDO_USER:-}" ]; then chown -R "$SUDO_USER" "$USER_HOME/AVMatrix-backups"; fi

echo "Reiniciando el servicio de audio (coreaudiod)."
killall coreaudiod 2>/dev/null || launchctl kickstart -k system/com.apple.audio.coreaudiod || true
echo "AV Matrix desinstalado. CoreAudio puede conservar la matriz y la frecuencia guardadas"
echo "en su almacenamiento interno; se reutilizan si se vuelve a instalar."
