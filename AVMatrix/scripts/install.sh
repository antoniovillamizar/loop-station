#!/bin/bash
# Instala AV Matrix de forma transaccional.
#
#   sudo scripts/install.sh               instala, reinicia coreaudiod y comprueba
#   sudo scripts/install.sh --no-restart  instala y verifica archivos; no reinicia coreaudiod
#   sudo scripts/install.sh --dry-run     solo valida y prepara en staging; no toca lo instalado
#   sudo scripts/install.sh --sign-identity=-   equivale a AVM_SIGN_IDENTITY=- (ver «Firma»)
#
# Pasos:
#   1. Valida los tres componentes (driver, app, avmctl): estructura, Info.plist,
#      arquitectura y firma. Si algo falla, no se toca nada instalado.
#   2. Prepara copias en staging junto a cada destino (mismo volumen).
#      Solo firma si hace falta y nunca sustituye una firma Developer ID válida
#      por una ad hoc. Comprueba las firmas en staging.
#   3. Respalda lo instalado en ~/AVMatrix-backups/<fecha>/.
#   4. Sustituye cada componente (rename). Si falla cualquier paso, restaura lo anterior.
#   5. Verifica los archivos instalados, reinicia coreaudiod y espera a que
#      `avmctl status` encuentre los tres dispositivos. Si no, restaura y devuelve error.
#
# Firma (variable AVM_SIGN_IDENTITY):
#   sin definir   se exige que los componentes ya tengan una firma válida (la de build.sh)
#   "-"           ad hoc solo para los componentes sin firma válida; una firma con
#                 identidad (p. ej. Developer ID) se conserva
#   "<identidad>" se firma con esa identidad lo que no esté ya firmado por ella
set -euo pipefail

# --- Configuración (las variables AVM_* permiten probar el script fuera de /Library) ---
HAL="${AVM_HAL_DIR:-/Library/Audio/Plug-Ins/HAL}"
APPS="${AVM_APPS_DIR:-/Applications}"
BIN="${AVM_BIN_DIR:-/usr/local/bin}"
CODESIGN="${CODESIGN:-codesign}"
PLUTIL="${PLUTIL:-plutil}"
LIPO="${LIPO:-lipo}"
XATTR="${XATTR:-xattr}"
CHOWN="${CHOWN:-chown}"
ARCH="${AVM_ARCH:-$(uname -m)}"
STATUS_TIMEOUT="${AVM_STATUS_TIMEOUT:-20}"
SIGN_IDENTITY="${AVM_SIGN_IDENTITY-}"
SIGN_IDENTITY_SET="${AVM_SIGN_IDENTITY+x}"
HAL_PLUGIN_TYPE="443ABAB8-E7B3-491A-B985-BEB9187030DB"

DO_RESTART=1
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --no-restart) DO_RESTART=0 ;;
        --dry-run)    DRY_RUN=1; DO_RESTART=0 ;;
        --sign-identity=*) SIGN_IDENTITY="${arg#--sign-identity=}"; SIGN_IDENTITY_SET=x ;;
        *) echo "Opción desconocida: $arg" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ] && [ -z "${AVM_ALLOW_NONROOT:-}" ]; then
    echo "Ejecute con sudo: sudo $0" >&2
    exit 1
fi

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
if [ -n "${AVM_SRC_DIR:-}" ]; then
    SRC="$AVM_SRC_DIR"
elif [ -d "$ROOT/build/mac/AVMatrix.driver" ]; then
    SRC="$ROOT/build/mac"
elif [ -d "$ROOT/dist/AVMatrix.driver" ]; then
    SRC="$ROOT/dist"
    echo "Aviso: se usa la compilación precompilada de dist/. Se recomienda scripts/build.sh."
else
    echo "ERROR: no hay compilación. Ejecute primero scripts/build.sh" >&2
    exit 1
fi

USER_HOME=$(eval echo "~${SUDO_USER:-$(id -un)}")
BACKUP_ROOT="${AVM_BACKUP_ROOT:-$USER_HOME/AVMatrix-backups}"
STAMP=$(date +%Y%m%d_%H%M%S)
BACKUP="$BACKUP_ROOT/$STAMP"
TAG="avm$$"

# Componentes: nombre | origen | destino | identificador de bundle ("" = ejecutable suelto)
NAMES=(driver app cli)
SRCS=("$SRC/AVMatrix.driver" "$SRC/AV Matrix.app" "$SRC/avmctl")
DESTS=("$HAL/AVMatrix.driver" "$APPS/AV Matrix.app" "$BIN/avmctl")
BUNDLE_IDS=("com.antoniovillamizar.avmatrix.driver" "com.antoniovillamizar.avmatrix.app" "")

declare -a STAGED=("" "" "")
declare -a OLD=("" "" "")
declare -a SWAPPED=(0 0 0)
declare -a SIGN_WITH=("" "" "")
declare -a ORIG_SIG=("" "" "")
SWAP_STARTED=0
RESTARTED=0
SUCCESS=0

log()  { echo "$*"; }
die()  { echo "ERROR: $*" >&2; exit 1; }

restart_coreaudiod() {
    if [ -n "${AVM_RESTART_CMD:-}" ]; then
        eval "$AVM_RESTART_CMD"
    else
        killall coreaudiod 2>/dev/null || launchctl kickstart -k system/com.apple.audio.coreaudiod
    fi
}

# Estado de firma: "invalid", "adhoc" o "identity:<autoridad principal>".
sig_state() {
    local p="$1" deep=""
    [ -d "$p" ] && deep="--deep"
    # (bash 3.2 de macOS: sin arrays vacíos bajo set -u)
    if ! "$CODESIGN" --verify --strict $deep "$p" >/dev/null 2>&1; then
        echo invalid; return
    fi
    local info
    info=$("$CODESIGN" -dvv "$p" 2>&1 || true)
    if echo "$info" | grep -q "^Signature=adhoc"; then
        echo adhoc
    else
        local auth
        auth=$(echo "$info" | grep -m1 "^Authority=" | sed 's/^Authority=//')
        if [ -n "$auth" ]; then echo "identity:$auth"; else echo adhoc; fi
    fi
}

rollback() {
    echo "Restaurando la instalación anterior..." >&2
    local i
    for i in 2 1 0; do
        local dest="${DESTS[$i]}"
        if [ "${SWAPPED[$i]}" = 1 ]; then
            rm -rf "$dest"
        fi
        if [ -n "${OLD[$i]}" ] && [ -e "${OLD[$i]}" ]; then
            if [ -e "$dest" ]; then rm -rf "$dest"; fi
            mv "${OLD[$i]}" "$dest" && echo "  restaurado: $dest" >&2
        elif [ "${SWAPPED[$i]}" = 1 ] && [ -e "$BACKUP/$(basename "$dest")" ]; then
            cp -Rp "$BACKUP/$(basename "$dest")" "$dest" && echo "  restaurado desde copia: $dest" >&2
        fi
    done
    if [ "$RESTARTED" = 1 ]; then
        echo "Reiniciando coreaudiod con la instalación anterior." >&2
        restart_coreaudiod || echo "  no se pudo reiniciar coreaudiod" >&2
    fi
}

on_exit() {
    local rc=$?
    set +e
    if [ "$SUCCESS" != 1 ] && [ "$SWAP_STARTED" = 1 ]; then
        rollback
    fi
    local i
    for i in 0 1 2; do
        if [ -n "${STAGED[$i]}" ] && [ -e "${STAGED[$i]}" ]; then rm -rf "${STAGED[$i]}"; fi
        if [ "$SUCCESS" = 1 ] && [ -n "${OLD[$i]}" ] && [ -e "${OLD[$i]}" ]; then rm -rf "${OLD[$i]}"; fi
    done
    if [ "$SUCCESS" != 1 ]; then
        echo "La instalación NO se completó (código $rc)." >&2
        [ "$rc" -eq 0 ] && rc=1
    fi
    exit "$rc"
}
trap on_exit EXIT

executable_of() {
    local i="$1" src="$2"
    if [ -z "${BUNDLE_IDS[$i]}" ]; then
        echo "$src"
    else
        local exe
        exe=$(grep -A1 "<key>CFBundleExecutable</key>" "$src/Contents/Info.plist" | sed -n 's/.*<string>\(.*\)<\/string>.*/\1/p')
        echo "$src/Contents/MacOS/$exe"
    fi
}

# --- 1. Validación de los tres componentes -----------------------------------
log "== 1. Validación (origen: $SRC) =="
for i in 0 1 2; do
    name="${NAMES[$i]}" src="${SRCS[$i]}"
    [ -e "$src" ] || die "$name: falta $src"
    if [ -n "${BUNDLE_IDS[$i]}" ]; then
        plist="$src/Contents/Info.plist"
        [ -f "$plist" ] || die "$name: falta Contents/Info.plist"
        "$PLUTIL" -lint "$plist" >/dev/null || die "$name: Info.plist inválido"
        grep -q "<string>${BUNDLE_IDS[$i]}</string>" "$plist" || die "$name: CFBundleIdentifier distinto de ${BUNDLE_IDS[$i]}"
        if [ "$name" = driver ]; then
            grep -q "$HAL_PLUGIN_TYPE" "$plist" || die "driver: Info.plist sin el tipo AudioServerPlugIn"
            grep -q "<string>AVMatrix_Create</string>" "$plist" || die "driver: Info.plist sin la fábrica AVMatrix_Create"
        fi
    fi
    exe=$(executable_of "$i" "$src")
    [ -f "$exe" ] && [ -x "$exe" ] || die "$name: ejecutable ausente o sin permiso de ejecución: $exe"
    archs=$("$LIPO" -archs "$exe" 2>/dev/null) || die "$name: no se pudieron leer las arquitecturas de $exe"
    echo " $archs " | grep -q " $ARCH " || die "$name: no contiene la arquitectura $ARCH (tiene: $archs)"

    st=$(sig_state "$src")
    ORIG_SIG[$i]="$st"
    if [ -z "$SIGN_IDENTITY_SET" ]; then
        [ "$st" != invalid ] || die "$name: firma inválida o ausente. Compile con scripts/build.sh o defina AVM_SIGN_IDENTITY"
        SIGN_WITH[$i]=""
    elif [ "$SIGN_IDENTITY" = "-" ]; then
        case "$st" in
            invalid)    SIGN_WITH[$i]="-" ;;
            identity:*) SIGN_WITH[$i]=""; log "  $name: se conserva la firma existente (${st#identity:}); no se sustituye por ad hoc" ;;
            *)          SIGN_WITH[$i]="" ;;
        esac
    else
        case "$st" in
            identity:*"$SIGN_IDENTITY"*) SIGN_WITH[$i]="" ;;
            *)                           SIGN_WITH[$i]="$SIGN_IDENTITY" ;;
        esac
    fi
    log "  $name: correcto (arquitecturas: $archs; firma: $st${SIGN_WITH[$i]:+; se firmará con '${SIGN_WITH[$i]}'})"
done

# --- 2. Staging ----------------------------------------------------------------
log "== 2. Preparación en staging =="
for i in 0 1 2; do
    name="${NAMES[$i]}" src="${SRCS[$i]}" dest="${DESTS[$i]}"
    dir=$(dirname "$dest")
    mkdir -p "$dir"
    stage="$dir/.$(basename "$dest").new.$TAG"
    STAGED[$i]="$stage"
    rm -rf "$stage"
    cp -Rp "$src" "$stage"
    # Quitar la cuarentena solo si está presente; cualquier otro error es fatal.
    while IFS= read -r -d '' f; do
        if "$XATTR" -p com.apple.quarantine "$f" >/dev/null 2>&1; then
            "$XATTR" -d com.apple.quarantine "$f"
        fi
    done < <(find "$stage" -print0)
    if [ "$name" = driver ]; then
        "$CHOWN" -R root:wheel "$stage"
        chmod -R go-w "$stage"
    elif [ "$name" = cli ]; then
        chmod 755 "$stage"
    fi
    if [ -n "${SIGN_WITH[$i]}" ]; then
        "$CODESIGN" --force --sign "${SIGN_WITH[$i]}" --timestamp=none "$stage" || die "$name: falló la firma con '${SIGN_WITH[$i]}'"
    fi
    st=$(sig_state "$stage")
    [ "$st" != invalid ] || die "$name: la firma no es válida en staging"
    case "${ORIG_SIG[$i]}" in
        identity:*) [ "$st" = "${ORIG_SIG[$i]}" ] || [ -n "${SIGN_WITH[$i]}" ] || die "$name: la firma cambió en staging ($st)" ;;
    esac
    if [ -n "${SIGN_WITH[$i]}" ] && [ "${SIGN_WITH[$i]}" != "-" ] && [ "$st" = adhoc ]; then
        die "$name: se pidió la identidad '${SIGN_WITH[$i]}' pero la firma resultante es ad hoc"
    fi
    log "  $name: listo ($st)"
done

if [ "$DRY_RUN" = 1 ]; then
    log "Prueba en seco correcta: los tres componentes son válidos. No se instaló nada."
    SUCCESS=1
    exit 0
fi

# --- 3. Respaldo -----------------------------------------------------------------
log "== 3. Respaldo =="
for i in 0 1 2; do
    dest="${DESTS[$i]}"
    if [ -e "$dest" ]; then
        mkdir -p "$BACKUP"
        cp -Rp "$dest" "$BACKUP/"
        diff -rq "$dest" "$BACKUP/$(basename "$dest")" >/dev/null || die "la copia de seguridad de $dest no coincide"
        log "  $dest -> $BACKUP/"
    fi
done
if [ -d "$BACKUP" ] && [ -n "${SUDO_USER:-}" ]; then "$CHOWN" -R "$SUDO_USER" "$BACKUP_ROOT"; fi

# --- 4. Sustitución --------------------------------------------------------------
log "== 4. Sustitución =="
SWAP_STARTED=1
for i in 0 1 2; do
    dest="${DESTS[$i]}"
    if [ -e "$dest" ]; then
        OLD[$i]="$(dirname "$dest")/.$(basename "$dest").old.$TAG"
        rm -rf "${OLD[$i]}"
        mv "$dest" "${OLD[$i]}"
    fi
    [ "${AVM_TEST_FAIL_AT:-}" != "swap:${NAMES[$i]}" ] || die "fallo simulado en la sustitución de ${NAMES[$i]}"
    mv "${STAGED[$i]}" "$dest"
    STAGED[$i]=""
    SWAPPED[$i]=1
    log "  instalado: $dest"
done

# --- 5. Verificación -------------------------------------------------------------
log "== 5. Verificación =="
for i in 0 1 2; do
    st=$(sig_state "${DESTS[$i]}")
    [ "$st" != invalid ] || die "${NAMES[$i]}: firma inválida en el destino"
    if [ -n "${BUNDLE_IDS[$i]}" ]; then
        "$PLUTIL" -lint "${DESTS[$i]}/Contents/Info.plist" >/dev/null || die "${NAMES[$i]}: Info.plist inválido en el destino"
    fi
done
log "  archivos instalados y firmas correctas"

if [ "$DO_RESTART" = 0 ]; then
    log "No se reinició coreaudiod (--no-restart). El driver se cargará en el próximo reinicio de"
    log "coreaudiod o del Mac. La comprobación de funcionamiento queda PENDIENTE: avmctl status"
    SUCCESS=1
    exit 0
fi

log "Reiniciando coreaudiod. El audio del Mac se cortará unos segundos."
RESTARTED=1
restart_coreaudiod || die "no se pudo reiniciar coreaudiod"

ok=0
for _ in $(seq 1 "$STATUS_TIMEOUT"); do
    if "$BIN/avmctl" status >/dev/null 2>&1; then ok=1; break; fi
    sleep 1
done
if [ "$ok" != 1 ]; then
    "$BIN/avmctl" status >&2 || true
    die "los dispositivos AV Matrix no aparecieron en ${STATUS_TIMEOUT} s"
fi
"$BIN/avmctl" status
SUCCESS=1
log "AV Matrix instalado y operativo."
