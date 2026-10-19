#!/bin/bash
# Comprueba que la app no formatea textos C con %s (macOS los interpreta con la
# codificación del sistema y «Música» sale como «M√∫sica») y que los nombres C
# de grupos y dispositivos pasan por UTF8() / stringWithUTF8String.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="${AVM_APP_SOURCE:-$HERE/../App/AVMatrixApp.m}"
PASS=0; FAIL=0
check() { local d="$1"; shift; if "$@"; then PASS=$((PASS+1)); else FAIL=$((FAIL+1)); echo "FALLO: $d"; fi; }

# Código sin comentarios de línea.
CODE=$(sed 's://.*$::' "$APP")

check "ningún %s en cadenas de formato de la app" bash -c '! grep -nE "@\"[^\"]*%s" <<< "$1"' _ "$CODE"
check "los nombres de grupo se convierten con UTF8()" bash -c '! grep -nE "k(Tx|Rx)Groups\[[^]]*\]\.name" <<< "$1" | grep -v "UTF8(k" | grep -q .' _ "$CODE"
check "las etiquetas de dispositivo se convierten con UTF8()" bash -c '! grep -n "avm_device_label(" <<< "$1" | grep -v "UTF8(avm_device_label" | grep -q .' _ "$CODE"
check "UTF8() usa stringWithUTF8String" grep -q "stringWithUTF8String:s" "$APP"
for f in "$APP" "$HERE/../Client/avm_client.c" "$HERE/../Client/avmctl.c"; do
    check "$(basename "$f") es UTF-8 válido" bash -c 'iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1' _ "$f"
done
check "«Música» está escrito en UTF-8 en los nombres de grupo" grep -q '{ "Música",' "$APP"
check "los alias se convierten con UTF8()" grep -q "return a ? UTF8(a) : nil;" "$APP"
check "los alias se guardan en forma compuesta NFC (tildes)" grep -q "precomposedStringWithCanonicalMapping" "$APP"
check "la ruta del archivo de nombres se pasa en UTF-8" grep -q "NamesFilePath().fileSystemRepresentation" "$APP"
check "un alias nunca se usa como cadena de formato" bash -c '! grep -nE "stringWithFormat:(ChannelAlias|RxChannelName|TxChannelName|a)[,)]" <<< "$1" | grep -q .' _ "$CODE"

# Panel de nombres: sin ciclo de retención y con tamaño explícito.
check "el panel guarda su NSPopover con referencia débil" grep -q "@property (weak) NSPopover\* popover;" "$APP"
check "ningún controlador del panel retenido fuera del popover" bash -c '! grep -q "AVMRenameController\* renamer" "$1"' _ "$APP"
check "el NSPopover recibe un contentSize explícito" grep -q "pop.contentSize = rc.preferredContentSize;" "$APP"
check "el tamaño se recalcula al mostrar un error" bash -c 'grep -A4 "^- (void)showMessage:" "$1" | grep -q "updateSize"' _ "$APP"
check "cada fila del panel tiene el ancho interior fijo" grep -q "\[row.widthAnchor constraintEqualToConstant:inner\].active = YES;" "$APP"
check "el ancho del panel no depende del texto" bash -c 'grep -A5 "^- (void)updateSize" "$1" | grep -q "s.width = kRenameW;"' _ "$APP"
check "el panel no toma el ancho de fittingSize" bash -c '! grep -q "MAX(s.width, kRenameW)" "$1"' _ "$APP"
check "el AVMController suelta el panel al cerrarse" bash -c 'grep -A5 "^- (void)popoverDidClose:" "$1" | grep -q "self.renamePopover = nil"' _ "$APP"

# Pantalla principal simplificada (revisión 11).
check "sin el distintivo «REFERENCIA LOCAL» en la matriz" bash -c '! grep -q "@\"REFERENCIA LOCAL\"" "$1" && ! grep -q "kRefBadgeW" "$1"' _ "$APP"
check "zona de receptores compacta (kLeftW ≤ 280)" bash -c 'w=$(sed -n "s/^static const CGFloat kLeftW = \([0-9.]*\);.*/\1/p" "$1"); [ -n "$w" ] && awk -v w="$w" "BEGIN{exit !(w <= 280)}"' _ "$APP"
check "la leyenda ya no está en la ventana principal" bash -c '! grep -q "Verde ✓ = conectado" "$1"' _ "$APP"
check "botón «Ayuda» en la barra superior" grep -q 'buttonWithTitle:@"Ayuda" target:self action:@selector(showHelp:)' "$APP"
check "menú Ayuda → Manual de AV Matrix" grep -q 'addItemWithTitle:@"Manual de AV Matrix" action:@selector(showHelp:)' "$APP"
check "ayuda emergente con el nombre completo de cada canal" grep -q "addToolTipRect:" "$APP"
check "el manual se convierte con UTF8() y nunca como formato" bash -c 'grep -q "UTF8(avm_manual_sections\[i\].body)" "$1" && ! grep -qE "stringWithFormat:UTF8\(avm_manual" "$1"' _ "$APP"
check "avm_manual.c es UTF-8 válido" bash -c 'iconv -f UTF-8 -t UTF-8 "$1" >/dev/null 2>&1' _ "$HERE/../App/avm_manual.c"

echo "Textos de la app: $PASS comprobaciones correctas, $FAIL fallos"
[ $FAIL -eq 0 ]
