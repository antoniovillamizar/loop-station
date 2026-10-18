# AV Matrix — tarjeta de audio virtual para macOS

AV Matrix es un driver de audio virtual (AudioServerPlugIn de CoreAudio) con una matriz de enrutamiento. Sirve para medir con **Smaart** a través de **LISTENTO**: la referencia sale hacia Colombia y el retorno vuelve al mismo dispositivo que Smaart tiene seleccionado.

La tarjeta **solo transporta y enruta audio**. No tiene ecualización, efectos, análisis ni delay ajustable. Smaart hace todo el análisis. El transporte usa un buffer interno, así que **la latencia no es cero** (ver «Retardo de transporte»).

> Estado: el código está compilado y las pruebas del núcleo pasan. **Todavía no se ha instalado ni probado en un Mac**: la compatibilidad con Smaart y LISTENTO está pendiente. Ver [`docs/INFORME_CODEX.md`](docs/INFORME_CODEX.md).

## 1. Qué aparece en macOS

| Dispositivo | Entradas | Salidas | Para qué |
| --- | --- | --- | --- |
| **AV Matrix Smaart** | 8 (`Entrada 1-8`) | 8 (`Salida 1-8`) | El único dispositivo que se selecciona en Smaart, para enviar y recibir a la vez |
| **AV Matrix LISTENTO** | 8 (`Envío 1-8`) | 8 (`Retorno 1-8`) | El que se selecciona en LISTENTO: captura el envío y reproduce el retorno |
| **AV Matrix Música** | — | 2 (`Música L/R`) | Salida del sistema para Apple Music, Spotify o Tidal, o salida de un DAW |

Los tres dispositivos comparten **un solo reloj** y una frecuencia (44,1, 48 o 96 kHz). Por eso el audio pasa de uno a otro sin deriva.

### Por qué LISTENTO tiene su propio dispositivo

Smaart ve **un único dispositivo** para enviar y recibir. Si LISTENTO usara ese mismo dispositivo, macOS **sumaría** en las mismas salidas el ruido rosa de Smaart y el retorno que reproduce LISTENTO. Es justo la mezcla involuntaria que hay que evitar. Con dos dispositivos separados, la matriz decide exactamente qué va a cada sitio.

Los canales de entrada son **entradas de línea genéricas**: no se declaran como micrófono.

## 2. La matriz

Fuentes (columnas, 18):

- **Smaart salida 1-8**: lo que Smaart envía, por ejemplo el ruido rosa.
- **LISTENTO retorno 1-8**: lo que LISTENTO recibe de Colombia.
- **Música L/R**: lo que se reproduce en «AV Matrix Música».

Destinos (filas, 16):

- **Smaart entrada 1-8**: lo que Smaart recibe.
- **LISTENTO envío 1-8**: lo que LISTENTO manda a Colombia.

Cada casilla conecta una fuente con un destino a **ganancia unidad** (0 dB). No hay ganancias variables.

- Si una fila tiene varias casillas marcadas, **suma** esas fuentes. La app lo avisa en naranja.
- **Bloqueado siempre:** *LISTENTO retorno → LISTENTO envío*. Formaría un lazo de realimentación a través de Colombia. La app muestra esas casillas en gris y el driver rechaza cualquier matriz que las contenga.
- **Preset predeterminado:** Smaart salida N → LISTENTO envío N, y LISTENTO retorno N → Smaart entrada N. La música queda sin conectar hasta que se asigne.
- La matriz y la frecuencia se guardan y se recuperan al reiniciar el Mac.

Ejemplo de **referencia para la función de transferencia**: Smaart genera ruido rosa por la salida 1, que va a Colombia por LISTENTO envío 1. Para tener también la referencia local en la entrada 8:

```
avmctl disconnect listento.out.8 smaart.in.8
avmctl connect smaart.out.1 smaart.in.8
```

En Smaart: medición = entrada 1 (retorno), referencia = entrada 8. El retardo de ida y vuelta por internet lo encuentra el buscador de delay de Smaart. AV Matrix no lo compensa.

## 3. Compilar

Requisitos: un Mac con las **Command Line Tools** de Xcode (no hace falta Xcode completo).

```bash
xcode-select --install          # solo si no están instaladas
cd AVMatrix
scripts/build.sh
```

`build.sh` muestra la versión de macOS y de las herramientas, y compila el driver, la app y `avmctl` como binarios universales (Apple Silicon + Intel). Los firma ad hoc y ejecuta las pruebas del núcleo. El resultado queda en `build/mac/`.

Si no se puede compilar en el Mac, `dist/` contiene una compilación precompilada. Se generó con compilación cruzada contra el SDK de macOS 15.5. `install.sh` la usa si no encuentra `build/mac/`, y la vuelve a firmar en el Mac.

## 4. Instalar

```bash
sudo scripts/install.sh
```

- Si ya hay una versión instalada, primero la copia en `~/AVMatrix-backups/<fecha>/`.
- Instala `AVMatrix.driver` en `/Library/Audio/Plug-Ins/HAL/`, la app en `/Applications/AV Matrix.app` y `avmctl` en `/usr/local/bin/`.
- Reinicia `coreaudiod`. **Todo el audio del Mac se corta unos segundos.** Cierre Smaart y LISTENTO antes.
- Al terminar muestra `avmctl status`.

Comprobación manual: en **Configuración de Audio MIDI** deben aparecer los tres dispositivos.

## 5. Desinstalar

```bash
sudo scripts/uninstall.sh
```

Guarda una copia de lo que elimina en `~/AVMatrix-backups/uninstall_<fecha>/` y reinicia `coreaudiod`.

## 6. Uso con Smaart y LISTENTO

1. Abra **AV Matrix** (la app) y pulse **Preset: Smaart ⇄ LISTENTO**, o ajuste la matriz.
2. **Smaart**: dispositivo de entrada y de salida = **AV Matrix Smaart**. El generador sale por la salida 1. Las mediciones usan las entradas 1-8, que reciben el retorno.
3. **LISTENTO**: para transmitir, entrada = **AV Matrix LISTENTO**, canales `Envío`. Para recibir el retorno, salida = **AV Matrix LISTENTO**, canales `Retorno`.
4. En Colombia reciben, reproducen en el equipo y devuelven la señal por LISTENTO.
5. **Música (opcional):** en Configuración del Sistema → Sonido → Salida, elija **AV Matrix Música**. Apple Music y Spotify siguen la salida del sistema; Tidal y los DAW permiten elegirla directamente. En la matriz, conecte `Música L/R` a los destinos que quiera, por ejemplo a LISTENTO envío 7-8.

Para que los sonidos del sistema no entren en la medición, AV Matrix Smaart y AV Matrix LISTENTO **no pueden ser la salida por defecto** del sistema. Ninguno de los tres puede ser el dispositivo de alertas.

**Permiso de micrófono:** macOS pide permiso de micrófono a cualquier aplicación que lea entradas de audio, aunque sean virtuales: Smaart, LISTENTO y Terminal si se usa `avmctl test`.

**Precisión por internet:** la calidad de la medición depende de LISTENTO (códec, pérdidas, variación del retardo). Use su modo sin compresión (PCM) si lo tiene. AV Matrix no puede garantizar ni comprobar esa parte.

## 7. avmctl (Terminal)

```
avmctl status                       dispositivos, frecuencia y formatos
avmctl show                         matriz actual
avmctl connect smaart.out.1 listento.in.1
avmctl disconnect listento.out.8 smaart.in.8
avmctl preset default | clear
avmctl rate 44100 | 48000 | 96000
avmctl format smaart in s24         (smaart|listento|music, in|out, f32|s32|s24|s16)
avmctl test                         autoprueba a la frecuencia actual
avmctl test-all                     autoprueba en 3 frecuencias x 4 formatos (~1 min)
```

`avmctl test` abre los tres dispositivos a la vez, como Smaart, LISTENTO y Apple Music. Envía un valor distinto por cada canal y comprueba **muestra por muestra** que cada destino recibe solo su fuente. Comprueba también que la música llega a sus destinos, que los retornos 7 y 8 sin ruta no aparecen en ningún sitio y que el driver rechaza el lazo. Por último, mide el retardo de transporte con un impulso. Al terminar restaura la matriz, la frecuencia y los formatos. **Cierre Smaart, LISTENTO y la música mientras se ejecuta**, porque el driver suma lo que envíen otras aplicaciones.

## 8. Frecuencias y formatos — qué admite realmente

**Frecuencias:** 44 100, 48 000 y 96 000 Hz. Los tres dispositivos cambian juntos.

**Formato que ven las aplicaciones (formato virtual):** siempre **32 bits coma flotante**. Así funciona CoreAudio: Smaart, LISTENTO y los demás programas intercambian audio en Float32 con el HAL.

**Formatos físicos** (se eligen por stream en Configuración de Audio MIDI o con `avmctl format`):

| Formato | Admitido | Comportamiento real |
| --- | --- | --- |
| 32 bits coma flotante | Sí (predeterminado) | Sin conversión: transparente bit a bit. No recorta por encima de 0 dBFS. |
| 32 bits **entero** | Sí | Se convierte a Float32 para transportarlo: **resolución efectiva de 24 bits** (mantisa de float), no de 32 bits enteros. Recorta a escala completa. |
| 24 bits entero (empaquetado, 3 bytes) | Sí | Ida y vuelta exacta. Recorta a escala completa. |
| 16 bits entero | Sí | Ida y vuelta exacta a 16 bits. Recorta a escala completa. |

En un dispositivo virtual no hay convertidores: elegir 16 o 24 bits solo **cuantiza** la señal. Para medir se recomienda **32 bits coma flotante**.

## 9. Retardo de transporte

Pasar audio de una salida a una entrada virtual requiere al menos un ciclo de buffer de CoreAudio. El retardo exacto depende del tamaño de buffer que usen Smaart y LISTENTO. `avmctl test` lo mide y lo muestra. El driver informa latencia 0 porque no tiene hardware; el retardo real del lazo lo resuelve el buscador de delay de Smaart.

## 10. Problemas

- **No aparecen los dispositivos:** `avmctl status`; luego
  `log show --last 5m --predicate 'process CONTAINS "coreaudiod" OR process CONTAINS "Core Audio Driver"' | grep -i avmatrix`.
- **Smaart no muestra «AV Matrix Smaart»:** ver «Riesgos» en el informe. El primer ajuste a probar es `canBeDefaultOutput` en `Driver/src/AVMatrixDriver.c`.
- **Silencio:** revise la matriz (`avmctl show`) y que los permisos de micrófono estén concedidos.

## Estructura

```
Driver/src/avm_core.[ch]      núcleo portable: matriz, buffers, formatos, reglas anti-lazo
Driver/src/AVMatrixDriver.c   AudioServerPlugIn (3 dispositivos, reloj común)
Driver/Info.plist
Client/avm_client.[ch]        acceso CoreAudio compartido
Client/avmctl.c               herramienta de Terminal y autoprueba
App/AVMatrixApp.m             app de la matriz (AppKit)
tests/test_core.c             pruebas unitarias del núcleo
scripts/                      build.sh, install.sh, uninstall.sh
dist/                         compilación precompilada (cruzada, SDK 15.5)
docs/INFORME_CODEX.md         informe de revisión
```
