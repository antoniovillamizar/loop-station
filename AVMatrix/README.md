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

La app **AV Matrix** muestra la matriz como pantalla principal, con la organización de Dante Controller y Dante Preset Creator:

- **Arriba, los transmisores (fuentes)**, con nombres verticales y agrupados. Cada grupo tiene una columna de cabecera con ⊟/⊞ para plegarlo o desplegarlo:
  - **Smaart**: Salida 1-8 (lo que Smaart envía, por ejemplo el ruido rosa).
  - **LISTENTO**: Retorno 1-8 (lo que LISTENTO recibe de Colombia).
  - **Música**: L/R (lo que se reproduce en «AV Matrix Música»).
- **A la izquierda, los receptores (destinos)**, agrupados y desplegables:
  - **Smaart**: Entrada 1-8 (lo que Smaart recibe). **Entrada 8 = REFERENCIA LOCAL**, con etiqueta naranja.
  - **LISTENTO**: Envío 1-8 (lo que LISTENTO manda a Colombia).
- **Casilla verde con ✓** = conexión activa. **Casilla vacía** = desconectada. **Casilla gris con ×** = bloqueada.
- Un punto verde en una cabecera o en un grupo plegado indica que hay conexiones dentro. Al hacer clic se despliega; nunca cambia conexiones.
- Al pasar el ratón se resaltan la fila y la columna, y abajo se lee la ruta completa (por ejemplo «Smaart Salida 1 → Smaart Entrada 8 (REFERENCIA LOCAL): conectado»).
- La ventana se ajusta al tamaño de la matriz, y el estado plegado o desplegado de cada grupo se recuerda.

Cada casilla conecta una fuente con un destino a **ganancia unidad** (0 dB). No hay ganancias variables.

- Si una fila tiene varias casillas marcadas, **suma** esas fuentes. La app lo indica con **Σ n** en naranja a la derecha de la fila.
- **Bloqueado siempre:** *LISTENTO retorno → LISTENTO envío*. Formaría un lazo de realimentación a través de Colombia. El driver rechaza cualquier matriz que lo contenga.
- La matriz y la frecuencia se guardan y se recuperan al reiniciar el Mac.

**Presets:**

| Preset | Conexiones |
| --- | --- |
| **Medición** (predeterminado; botón «Preset medición», `avmctl preset default`) | Smaart Salida N → LISTENTO Envío N (1-8) · LISTENTO Retorno N → Smaart Entrada N (1-7) · **Smaart Salida 1 → Smaart Entrada 8 (REFERENCIA LOCAL)**, sin el retorno 8 de LISTENTO |
| **Directo 8×8** (botón «Directo 8×8», `avmctl preset direct`) | Smaart Salida N → LISTENTO Envío N y LISTENTO Retorno N → Smaart Entrada N, de 1 a 8, sin referencia local |
| **Vaciar** (`avmctl preset clear`) | Ninguna |

Con el preset de medición, Smaart genera ruido rosa por la Salida 1. La señal va a Colombia por LISTENTO Envío 1 y también llega a la Entrada 8 como referencia local. En Smaart: medición = Entrada 1 (retorno), referencia = Entrada 8. El retardo de ida y vuelta por internet lo encuentra el buscador de delay de Smaart; AV Matrix no lo compensa. Smaart ve el canal 8 con el nombre «Entrada 8 (referencia local)».

### Medidores de nivel

Cada canal tiene, junto a su nombre, una **barra de nivel**, la **lectura de pico en dBFS** y un **indicador rojo de saturación**:

| Dónde | Qué mide |
| --- | --- |
| **Transmisores (arriba, bajo cada nombre vertical):** Smaart Salidas 1-8, LISTENTO Retornos 1-8, Música L/R | La señal que la aplicación envía, **antes de la matriz**. Si varias apps usan la misma salida, incluye la mezcla que hace macOS |
| **Receptores (izquierda, a la derecha de cada nombre):** Smaart Entradas 1-8, LISTENTO Envíos 1-8 | La señal **realmente entregada**, después del enrutamiento y de la **suma** de las conexiones. No repite el medidor de la fuente: una entrada sin conexiones marca silencio aunque la fuente tenga señal |

- **Escala:** de -60 a 0 dBFS. Verde hasta -18, amarillo hasta -6 y naranja hasta 0. Un destino que suma por encima de 0 dBFS muestra valores positivos (por ejemplo +3,2).
- **Saturación:** el LED rojo se enciende cuando la muestra es de escala completa en su formato: el código máximo o el mínimo en los formatos enteros (con el mismo redondeo que la entrega), y ±1,0 en Float32. Vale igual para fuentes y destinos; una suma de 1,0 o más siempre satura. Se mantiene 2 s.
- **«—»** = no llega audio a ese canal (la app no lo usa o el audio se detuvo; tarda como mucho 0,25 s). **«−∞»** = llega audio, pero es silencio digital.
- El pico se calcula por ventanas de 50 ms; un pico antiguo desaparece en 0,1 s.
- **La medición no modifica el audio.** El driver observa los mismos valores que ya transporta: sin copias, sin procesamiento, sin cambios de ganancia ni retardo, y sin bloquear el hilo de audio (solo operaciones atómicas). Las pruebas comprueban que lo transportado es idéntico bit a bit con medición y sin ella.
- **La app no abre audio.** Solo lee 15 veces por segundo la propiedad `'metr'` del driver, y únicamente con la ventana visible. Abrirla o cerrarla no afecta a Smaart ni a LISTENTO.
- En la Terminal: `avmctl meters` (una lectura) o `avmctl meters --watch`.

## 3. Compilar

Requisitos: un Mac con las **Command Line Tools** de Xcode (no hace falta Xcode completo).

```bash
xcode-select --install          # solo si no están instaladas
cd AVMatrix
scripts/build.sh
```

`build.sh` muestra la versión de macOS y de las herramientas, y compila el driver, la app y `avmctl` como binarios universales (Apple Silicon + Intel). Los firma, verifica las firmas y ejecuta las pruebas (núcleo, frecuencia e instalador). El resultado queda en `build/mac/`.

**Icono de la app:** `build.sh` incluye `App/Resources/AVMatrix.icns`, declarado como `CFBundleIconFile` en `App/Info.plist`. Si ese archivo no existe, lo importa automáticamente de la app ya instalada (`/Applications/AV Matrix.app`) y lo deja en `App/Resources/` para las siguientes compilaciones. Si no lo encuentra en ninguna parte, se detiene para no generar una app con el icono genérico; `AVM_ALLOW_GENERIC_ICON=1` permite continuar sin él. También acepta `AVM_ICON_SOURCE=<archivo.icns>`. El instalador conserva el icono instalado si el paquete no trae uno.

Para firmar con una identidad propia (por ejemplo Developer ID): `AVM_SIGN_IDENTITY="Developer ID Application: …" scripts/build.sh`. Sin esa variable se firma ad hoc.

Si no se puede compilar en el Mac, `dist/` contiene una compilación precompilada. Se generó con compilación cruzada contra el SDK de macOS 15.5. Solo lleva la firma del enlazador, así que el instalador la rechaza salvo que se elija la firma de forma explícita (ver «Instalar»).

## 4. Instalar

```bash
sudo scripts/install.sh --dry-run      # solo comprueba; no toca nada instalado
sudo scripts/install.sh                # instala, reinicia coreaudiod y comprueba
sudo scripts/install.sh --no-restart   # instala sin reiniciar coreaudiod
```

La instalación es transaccional:

1. **Valida los tres componentes** antes de tocar nada: Info.plist, identificador, tipo AudioServerPlugIn y fábrica del driver, ejecutables, arquitectura del Mac y firma.
2. **Prepara las copias en staging** junto a cada destino y comprueba allí las firmas.
3. **Respalda** lo instalado en `~/AVMatrix-backups/<fecha>/` y verifica la copia.
4. **Sustituye** el driver (`/Library/Audio/Plug-Ins/HAL/`), la app (`/Applications/AV Matrix.app`) y `avmctl` (`/usr/local/bin/`).
5. **Reinicia `coreaudiod`** (todo el audio del Mac se corta unos segundos; cierre Smaart y LISTENTO antes) y espera a que `avmctl status` encuentre los tres dispositivos.

Si **cualquier paso falla**, restaura la versión anterior (y reinicia `coreaudiod` con ella si ya lo había reiniciado) y termina con error. Con `--no-restart` verifica los archivos y deja **pendiente** la comprobación de funcionamiento.

**Firma:**

| Elección | Qué hace |
| --- | --- |
| Nada (recomendado tras `build.sh`) | Exige que los tres componentes ya tengan firma válida; no firma nada |
| `--sign-identity=-` (o `AVM_SIGN_IDENTITY=-`) | Firma ad hoc **solo** lo que no tenga firma válida (necesario para `dist/`). Una firma Developer ID válida **se conserva** |
| `--sign-identity="Developer ID Application: …"` | Firma con esa identidad lo que no esté ya firmado por ella |

Los errores de firma detienen la instalación; nunca se ignoran.

Comprobación manual: en **Configuración de Audio MIDI** deben aparecer los tres dispositivos.

## 5. Desinstalar

```bash
sudo scripts/uninstall.sh
```

Guarda una copia de lo que elimina en `~/AVMatrix-backups/uninstall_<fecha>/` y reinicia `coreaudiod`.

## 6. Uso con Smaart y LISTENTO

1. Abra **AV Matrix** (la app) y pulse **Preset medición**, o ajuste la matriz.
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
avmctl preset default | direct | clear   (default = medición)
avmctl rate 44100 | 48000 | 96000
avmctl recover                      repara una frecuencia inconsistente (sin audio activo)
avmctl meters [--watch]             medidores de nivel de los 34 canales (no abre audio)
avmctl format smaart in s24         (smaart|listento|music, in|out, f32|s32|s24|s16)
avmctl test                         autoprueba a la frecuencia actual
avmctl test-all                     autoprueba en 3 frecuencias x 4 formatos (~1 min)
```

`avmctl test` abre los tres dispositivos a la vez, como Smaart, LISTENTO y Apple Music. Envía un valor distinto por cada canal y comprueba **muestra por muestra** que cada destino recibe solo su fuente. Comprueba también que la música llega a sus destinos, que los retornos 7 y 8 sin ruta no aparecen en ningún sitio y que el driver rechaza el lazo. Por último, mide el retardo de transporte con un impulso. Al terminar restaura la matriz, la frecuencia y los formatos. **Cierre Smaart, LISTENTO y la música mientras se ejecuta**, porque el driver suma lo que envíen otras aplicaciones.

## 8. Frecuencias y formatos — qué admite realmente

**Frecuencias:** 44 100, 48 000 y 96 000 Hz. Los tres dispositivos cambian juntos.

**El cambio de frecuencia solo se admite sin audio activo** en ninguno de los tres dispositivos. Detenga el audio en Smaart, cierre LISTENTO y pare la música, y luego cambie la frecuencia (en la app, con `avmctl rate` o en Smaart). Si hay audio activo, el cambio se rechaza con un aviso. Así ningún dispositivo funciona nunca a una frecuencia distinta de la que macOS cree. Si macOS cancela un cambio a medias, el driver vuelve a la frecuencia anterior en los tres.

Si aun así los dispositivos pudieran haber quedado a frecuencias distintas (por ejemplo, macOS cancela también la vuelta atrás o no la completa), AV Matrix **no deja arrancar el audio** en ninguno de los tres hasta repararlo. `avmctl status` lo indica y la app muestra un aviso en rojo con el botón **«Reparar frecuencia»**. Para repararlo, sin audio activo, use ese botón o `avmctl recover`: vuelve a aplicar la frecuencia actual en los tres dispositivos.

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
Driver/src/avm_rate.[ch]      transacción de cambio de frecuencia (portable, probada)
Driver/src/AVMatrixDriver.c   AudioServerPlugIn (3 dispositivos, reloj común)
Driver/Info.plist
Client/avm_client.[ch]        acceso CoreAudio compartido
Client/avmctl.c               herramienta de Terminal y autoprueba
App/AVMatrixApp.m             app de la matriz (AppKit)
tests/test_core.c             pruebas del núcleo (matriz, buffers, formatos, concurrencia)
tests/test_rate.c             pruebas de la transacción de frecuencia
tests/test_install.sh         pruebas del instalador con herramientas simuladas (tests/fakes)
tests/test_build_script.sh    ejecuta build.sh con una cadena de herramientas simulada
scripts/                      build.sh, install.sh, uninstall.sh
dist/                         compilación precompilada (cruzada, SDK 15.5)
docs/INFORME_CODEX.md         informe de revisión
```
