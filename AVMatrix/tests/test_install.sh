#!/bin/bash
# Pruebas de scripts/install.sh (correcciones Codex #3 y #4) con herramientas
# de macOS simuladas (tests/fakes) y directorios temporales. No toca el sistema.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
FAKES="$HERE/fakes"
INSTALL="${AVM_INSTALL_SCRIPT:-$ROOT/scripts/install.sh}"
PASS=0
FAIL=0

check() {
    local desc="$1"; shift
    if "$@"; then PASS=$((PASS + 1)); else FAIL=$((FAIL + 1)); echo "FALLO: $desc"; fi
}

new_env() {
    T=$(mktemp -d)
    mkdir -p "$T/inst/HAL" "$T/inst/Applications" "$T/inst/bin" "$T/backups"
    : > "$T/restarts"
    : > "$T/signlog"
}

# make_dist <dir> <versión> <firma driver> <firma app> <firma cli>
# firma: adhoc | id:<autoridad> | invalid
make_dist() {
    local d="$1" v="$2"
    rm -rf "$d"; mkdir -p "$d"
    mkdir -p "$d/AVMatrix.driver/Contents/MacOS" "$d/AV Matrix.app/Contents/MacOS"
    cp "$ROOT/Driver/Info.plist" "$d/AVMatrix.driver/Contents/Info.plist"
    cp "$ROOT/App/Info.plist" "$d/AV Matrix.app/Contents/Info.plist"
    printf '#ARCHS=x86_64 arm64\nversion=%s\n' "$v" > "$d/AVMatrix.driver/Contents/MacOS/AVMatrix"
    printf '#ARCHS=x86_64 arm64\nversion=%s\n' "$v" > "$d/AV Matrix.app/Contents/MacOS/AVMatrix"
    cat > "$d/avmctl" <<EOF
#!/bin/bash
#ARCHS=x86_64 arm64
# version=$v
if [ "\${1:-}" = status ]; then
    [ "\${FAKE_STATUS_FAIL_VERSION:-}" != "$v" ] || { echo "dispositivos no encontrados" >&2; exit 1; }
    echo "AV Matrix $v presente"
fi
exit 0
EOF
    chmod +x "$d/AVMatrix.driver/Contents/MacOS/AVMatrix" "$d/AV Matrix.app/Contents/MacOS/AVMatrix" "$d/avmctl"
    set_sig "$d/AVMatrix.driver" "$3"
    set_sig "$d/AV Matrix.app" "$4"
    set_sig "$d/avmctl" "$5"
}

set_sig() {
    local p="$1" s="$2"
    if [ -d "$p" ]; then
        rm -rf "$p/Contents/_CodeSignature"
        [ "$s" = invalid ] || { mkdir -p "$p/Contents/_CodeSignature"; echo "$s" > "$p/Contents/_CodeSignature/fake"; }
    else
        grep -v "^#SIG=" "$p" > "$p.tmp"; cat "$p.tmp" > "$p"; rm -f "$p.tmp"
        [ "$s" = invalid ] || echo "#SIG=$s" >> "$p"
    fi
}

sig_of() { "$FAKES/codesign" -dvv "$1" 2>&1 | grep -E "^(Signature|Authority)=" | head -1; }

run_install() {
    env AVM_HAL_DIR="$T/inst/HAL" AVM_APPS_DIR="$T/inst/Applications" AVM_BIN_DIR="$T/inst/bin" \
        AVM_SRC_DIR="$T/dist" AVM_BACKUP_ROOT="$T/backups" AVM_ALLOW_NONROOT=1 \
        CODESIGN="$FAKES/codesign" PLUTIL="$FAKES/plutil" LIPO="$FAKES/lipo" XATTR="$FAKES/xattr" CHOWN="$FAKES/chown" \
        AVM_ARCH="${ARCH_OVERRIDE:-arm64}" AVM_RESTART_CMD="echo restart >> '$T/restarts'" AVM_STATUS_TIMEOUT=2 \
        FAKE_LOG="$T/signlog" "$@" bash "$INSTALL" ${INSTALL_ARGS:-} > "$T/out" 2>&1
}

# Huella de todo lo instalado (portátil: Linux y macOS).
hash_file() { if command -v sha256sum >/dev/null 2>&1; then sha256sum < "$1"; else shasum -a 256 < "$1"; fi; }
snapshot() {
    (cd "$T/inst" && find . | LC_ALL=C sort | while IFS= read -r f; do
        if [ -f "$f" ]; then echo "$f $(hash_file "$f")"; else echo "$f"; fi
    done)
}
leftovers() { find "$T/inst" -name ".*.new.*" -o -name ".*.old.*" | grep -q .; }
installed_version() { grep -m1 "version" "$T/inst/bin/avmctl" | sed 's/.*version=//'; }
no_leftovers() { ! leftovers; }
file_has() { grep -q "$2" "$1"; }

# Instala v1 como estado previo.
seed_v1() {
    make_dist "$T/dist" v1 adhoc adhoc adhoc
    INSTALL_ARGS="--no-restart" run_install || { echo "no se pudo preparar v1"; cat "$T/out"; return 1; }
    : > "$T/signlog"
}

# 1. Instalación nueva sin reinicio
new_env
make_dist "$T/dist" v1 adhoc adhoc adhoc
INSTALL_ARGS="--no-restart" run_install; rc=$?
check "instalación nueva: termina bien" [ $rc -eq 0 ]
check "instalación nueva: driver presente" [ -f "$T/inst/HAL/AVMatrix.driver/Contents/Info.plist" ]
check "instalación nueva: app presente" [ -d "$T/inst/Applications/AV Matrix.app" ]
check "instalación nueva: avmctl presente" [ -x "$T/inst/bin/avmctl" ]
check "instalación nueva: sin restos de staging" no_leftovers
check "instalación nueva: no reinicia coreaudiod con --no-restart" [ ! -s "$T/restarts" ]
check "instalación nueva: avisa que la comprobación queda pendiente" file_has "$T/out" "PENDIENTE"
check "instalación nueva: firmas válidas no se tocan" [ ! -s "$T/signlog" ]
rm -rf "$T"

# 2. Actualización con reinicio y comprobación correcta
new_env; seed_v1
make_dist "$T/dist" v2 adhoc adhoc adhoc
run_install; rc=$?
check "actualización: termina bien" [ $rc -eq 0 ]
check "actualización: queda v2" [ "$(installed_version)" = v2 ]
check "actualización: respaldo de v1" bash -c "grep -rq 'version=v1' '$T/backups'"
check "actualización: reinicia coreaudiod una vez" [ "$(wc -l < "$T/restarts")" -eq 1 ]
check "actualización: sin restos" no_leftovers
rm -rf "$T"

# 3. Firma inválida sin identidad: no toca nada
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 adhoc invalid adhoc
run_install; rc=$?
check "firma inválida: devuelve error" [ $rc -ne 0 ]
check "firma inválida: lo instalado queda idéntico" [ "$(snapshot)" = "$before" ]
check "firma inválida: no firma nada" [ ! -s "$T/signlog" ]
check "firma inválida: no reinicia coreaudiod" [ ! -s "$T/restarts" ]
check "firma inválida: mensaje claro" file_has "$T/out" "firma inválida"
rm -rf "$T"

# 4. Developer ID válida + AVM_SIGN_IDENTITY=- : se conserva
new_env; seed_v1
make_dist "$T/dist" v2 "id:Developer ID Application: Antonio (TEAM1)" "id:Developer ID Application: Antonio (TEAM1)" adhoc
run_install AVM_SIGN_IDENTITY=-; rc=$?
check "Developer ID: termina bien" [ $rc -eq 0 ]
check "Developer ID: no se vuelve a firmar ad hoc" [ ! -s "$T/signlog" ]
check "Developer ID: el driver instalado conserva su autoridad" \
    [ "$(sig_of "$T/inst/HAL/AVMatrix.driver")" = "Authority=Developer ID Application: Antonio (TEAM1)" ]
check "Developer ID: lo indica en la salida" file_has "$T/out" "se conserva la firma existente"
rm -rf "$T"

# 5. Sin firma + AVM_SIGN_IDENTITY=- : se firma en staging, sin modificar el origen
new_env; seed_v1
make_dist "$T/dist" v2 invalid adhoc adhoc
run_install AVM_SIGN_IDENTITY=-; rc=$?
check "ad hoc explícito: termina bien" [ $rc -eq 0 ]
check "ad hoc explícito: firma solo el componente sin firma" [ "$(wc -l < "$T/signlog")" -eq 1 ]
check "ad hoc explícito: firma la copia de staging" bash -c "grep -q 'sign - .*\.new\.' '$T/signlog'"
check "ad hoc explícito: el origen no se modifica" [ ! -e "$T/dist/AVMatrix.driver/Contents/_CodeSignature/fake" ]
check "ad hoc explícito: el instalado queda firmado" [ "$(sig_of "$T/inst/HAL/AVMatrix.driver")" = "Signature=adhoc" ]
rm -rf "$T"

# 5b. La opción --sign-identity=- equivale a la variable
new_env
make_dist "$T/dist" v2 invalid adhoc adhoc
INSTALL_ARGS="--no-restart --sign-identity=-" run_install; rc=$?
check "--sign-identity=-: termina bien" [ $rc -eq 0 ]
check "--sign-identity=-: firma el componente sin firma" [ "$(wc -l < "$T/signlog")" -eq 1 ]
rm -rf "$T"

# 6. Identidad elegida: se firma con ella lo que no lo esté
new_env; seed_v1
make_dist "$T/dist" v2 adhoc adhoc adhoc
run_install "AVM_SIGN_IDENTITY=Developer ID Application: Antonio (TEAM1)"; rc=$?
check "identidad elegida: termina bien" [ $rc -eq 0 ]
check "identidad elegida: firma los tres" [ "$(wc -l < "$T/signlog")" -eq 3 ]
check "identidad elegida: el instalado lleva esa identidad" \
    [ "$(sig_of "$T/inst/bin/avmctl")" = "Authority=Developer ID Application: Antonio (TEAM1)" ]
rm -rf "$T"

# 7. Error de firma: no se ignora
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 invalid adhoc adhoc
run_install AVM_SIGN_IDENTITY=- FAKE_SIGN_FAIL=1; rc=$?
check "error de firma: devuelve error" [ $rc -ne 0 ]
check "error de firma: lo instalado queda idéntico" [ "$(snapshot)" = "$before" ]
check "error de firma: sin restos de staging" no_leftovers
rm -rf "$T"

# 8. Comprobación de funcionamiento fallida tras reiniciar: restaura v1
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 adhoc adhoc adhoc
run_install FAKE_STATUS_FAIL_VERSION=v2; rc=$?
check "no operativa: devuelve error" [ $rc -ne 0 ]
check "no operativa: v1 restaurada exactamente" [ "$(snapshot)" = "$before" ]
check "no operativa: reinicia de nuevo con la versión anterior" [ "$(wc -l < "$T/restarts")" -eq 2 ]
check "no operativa: sin restos" no_leftovers
rm -rf "$T"

# 9. Fallo a mitad de la sustitución (driver ya cambiado, falla la app)
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 adhoc adhoc adhoc
run_install AVM_TEST_FAIL_AT=swap:app; rc=$?
check "fallo en sustitución: devuelve error" [ $rc -ne 0 ]
check "fallo en sustitución: v1 restaurada exactamente" [ "$(snapshot)" = "$before" ]
check "fallo en sustitución: sin restos" no_leftovers
rm -rf "$T"

# 10. Fallo en sustitución sin instalación previa: no deja nada a medias
new_env
make_dist "$T/dist" v2 adhoc adhoc adhoc
run_install AVM_TEST_FAIL_AT=swap:cli; rc=$?
check "fallo sin previa: devuelve error" [ $rc -ne 0 ]
check "fallo sin previa: no queda driver" [ ! -e "$T/inst/HAL/AVMatrix.driver" ]
check "fallo sin previa: no queda app" [ ! -e "$T/inst/Applications/AV Matrix.app" ]
rm -rf "$T"

# 11. Info.plist inválido / arquitectura ausente / ejecutable ausente
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 adhoc adhoc adhoc
echo "BROKEN" >> "$T/dist/AVMatrix.driver/Contents/Info.plist"
run_install; rc=$?
check "plist inválido: devuelve error" [ $rc -ne 0 ]
check "plist inválido: nada cambia" [ "$(snapshot)" = "$before" ]
make_dist "$T/dist" v2 adhoc adhoc adhoc
ARCH_OVERRIDE=ppc run_install; rc=$?
check "arquitectura ausente: devuelve error" [ $rc -ne 0 ]
check "arquitectura ausente: nada cambia" [ "$(snapshot)" = "$before" ]
rm -f "$T/dist/AV Matrix.app/Contents/MacOS/AVMatrix"
run_install; rc=$?
check "ejecutable ausente: devuelve error" [ $rc -ne 0 ]
check "ejecutable ausente: nada cambia" [ "$(snapshot)" = "$before" ]
make_dist "$T/dist" v2 adhoc adhoc adhoc
P="$T/dist/AVMatrix.driver/Contents/Info.plist"; sed 's/AVMatrix_Create/OtraFabrica/' "$P" > "$P.tmp" && mv "$P.tmp" "$P"
run_install; rc=$?
check "fábrica ausente: devuelve error" [ $rc -ne 0 ]
check "fábrica ausente: nada cambia" [ "$(snapshot)" = "$before" ]
rm -rf "$T"

# 12. Prueba en seco
new_env; seed_v1; before=$(snapshot)
make_dist "$T/dist" v2 adhoc adhoc adhoc
INSTALL_ARGS="--dry-run" run_install; rc=$?
check "prueba en seco: termina bien" [ $rc -eq 0 ]
check "prueba en seco: nada cambia" [ "$(snapshot)" = "$before" ]
check "prueba en seco: sin respaldo nuevo" [ -z "$(ls -A "$T/backups")" ]
check "prueba en seco: no reinicia" [ ! -s "$T/restarts" ]
rm -rf "$T"

echo "Instalador: $PASS comprobaciones correctas, $FAIL fallos"
[ "$FAIL" -eq 0 ]
