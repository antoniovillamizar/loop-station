# AV Matrix — informe para revisión de Codex

Fecha: 18 de octubre de 2026. Proyecto nuevo e independiente en `AVMatrix/`. No modifica ningún archivo existente del repositorio, así que no hubo nada que respaldar. Los scripts de instalación respaldan lo que reemplazan en el Mac.

## 1. Resumen

AudioServerPlugIn de CoreAudio, escrito en C, que publica tres dispositivos virtuales con un reloj común y una matriz de enrutamiento. Incluye:

- una app de matriz (Objective-C/AppKit);
- una herramienta de Terminal con autoprueba (`avmctl`);
- scripts de compilación, instalación y desinstalación.

**Estado honesto:** el código compila y enlaza para macOS (arm64 y x86_64) y las pruebas del núcleo pasan. **No se ha ejecutado en ningún Mac.** No hay ninguna prueba hecha con Smaart, LISTENTO, Apple Music ni coreaudiod.

## 2. Verificación del sistema y elección de la implementación

| Aspecto | Hecho |
| --- | --- |
| Sistema indicado | macOS «Golden Gate» 27.0.1 |
| Sistema de esta sesión | **Linux** (Ubuntu, contenedor en la nube). **No se pudo verificar el Mac de Antonio.** `scripts/build.sh` imprime `sw_vers`, la arquitectura y las herramientas como primer paso, para registrar el sistema real |
| Herramientas aquí | clang 18, lld 18 (`ld64.lld`), gcc; sin Xcode ni Swift para macOS |
| SDK usado para comprobar | Cabeceras y stubs `.tbd` de **macOS SDK 15.5**, descargados de un espejo público solo para compilar. En el Mac, `build.sh` usa el SDK instalado (`xcrun`) |
| Destino mínimo | macOS 12.0, binarios universales arm64 + x86_64 |

**Por qué AudioServerPlugIn:** es la API pública de Apple para dispositivos de audio virtuales en espacio de usuario. Es la misma familia que usan BlackHole y Loopback. AudioDriverKit (DriverKit) requiere entitlements que Apple concede a desarrolladores aprobados y una System Extension firmada, desproporcionado para uso privado. AudioServerPlugIn se instala copiando el bundle en `/Library/Audio/Plug-Ins/HAL` y reiniciando `coreaudiod`.

**Supuesto pendiente:** que macOS 27 siga cargando AudioServerPlugIns de terceros firmados ad hoc, como hace macOS 15 y anteriores. No se pudo comprobar. Si macOS 27 exige firma Developer ID, `build.sh` acepta `AVM_SIGN_IDENTITY="Developer ID Application: …"`.

## 3. Arquitectura

### 3.1 Dispositivos (`Driver/src/AVMatrixDriver.c:74`)

| Dispositivo | UID | Entradas | Salidas | Por defecto del sistema |
| --- | --- | --- | --- | --- |
| AV Matrix Smaart | `AVMatrix_Smaart_UID` | 8 «Entrada N» | 8 «Salida N» | No |
| AV Matrix LISTENTO | `AVMatrix_Listento_UID` | 8 «Envío N» | 8 «Retorno N» | No |
| AV Matrix Música | `AVMatrix_Music_UID` | — | 2 «Música L/R» | Sí, como salida; no como dispositivo de alertas |

- Terminal de stream: `kAudioStreamTerminalTypeLine` en las dos direcciones (`AVMatrixDriver.c:936`). Las entradas no se declaran como micrófono.
- Transporte `kAudioDeviceTransportTypeVirtual`. Mismo `ClockDomain` en los tres.

**Desviación consciente sobre el «único dispositivo»:** Smaart ve un único dispositivo (AV Matrix Smaart) y lo usa para enviar y recibir. LISTENTO usa un segundo dispositivo. Si LISTENTO usara el de Smaart, el HAL de macOS sumaría las salidas de ambos clientes (ruido rosa + retorno) en los mismos canales antes de que el driver las reciba. Es justo la mezcla involuntaria que el pedido prohíbe. **Codex: confirmar que esta lectura del requisito es aceptable para Antonio.**

### 3.2 Matriz (`Driver/src/avm_core.[ch]`)

- 18 fuentes: Smaart salida 1-8, LISTENTO retorno 1-8, Música L/R.
- 16 destinos: Smaart entrada 1-8, LISTENTO envío 1-8.
- Cruces binarios a ganancia unidad. Varias fuentes en un destino se suman en float.
- **Regla anti-lazo** (`avm_core.c:61`): prohibido LISTENTO retorno → LISTENTO envío. Se aplica en tres sitios:
  - el driver rechaza la matriz completa (`AVMatrixDriver.c:520`, `avm_matrix_is_valid`);
  - el motor ignora el cruce aunque llegue a la matriz (`avm_engine_read`);
  - la UI deshabilita la casilla.
- Permitido a propósito: Smaart salida → Smaart entrada (referencia en bucle local). No es realimentación, porque Smaart no reenvía su entrada a su salida.
- Preset predeterminado: Smaart salida N → LISTENTO envío N; LISTENTO retorno N → Smaart entrada N. La música queda sin conectar.
- Se guarda con `WriteToStorage` (claves `matrix`, `sample_rate`, `format_stream_N`) y se valida al cargar.
- Control desde fuera: propiedad personalizada `'mtrx'` (CFData de 16×18 bytes, fila = destino), publicada en el plug-in y en los tres dispositivos (`kAudioObjectPropertyCustomPropertyInfoList`).

### 3.3 Transporte y reloj

- Un buffer circular float32 de 65 536 frames por fuente. Ventana válida: los 32 768 frames más recientes escritos de cada grupo de fuentes.
- `WriteMix` escribe en la posición `mOutputTime.mSampleTime`; `ReadInput` lee en `mInputTime.mSampleTime` y suma según la matriz.
- **Sin datos viejos** (`avm_core.c:251-341`):
  - lo que está fuera de `[max(written_end - ventana, valid_begin), written_end)` se lee como silencio;
  - si una aplicación deja de enviar y vuelve, el hueco se borra al escribir;
  - cuando el último cliente de los tres dispositivos se detiene y otro arranca, `StartIO` vacía el motor y crea una nueva línea de tiempo (`AVMatrixDriver.c:1114`).
- **Reloj común** (`AVMatrixDriver.c:1151`): `GetZeroTimeStamp` usa una sola ancla para los tres dispositivos (periodo 16 384 frames). Por eso la posición en frames coincide entre Smaart, LISTENTO y Música.
- **Cambio de frecuencia:** se pide `RequestDeviceConfigurationChange` en los tres dispositivos (`AVMatrixDriver.c:493`). Al aplicarlo, se recalcula el ancla para conservar la posición actual (`AVMatrixDriver.c:461`).
- La matriz usa un seqlock: un solo escritor (con mutex) y lectores en tiempo real sin bloqueo.
- El hilo de IO no reserva memoria ni toma mutex, salvo el mutex corto de `GetZeroTimeStamp`, que es el mismo patrón que el ejemplo NullAudio de Apple.

### 3.4 Formatos

- **Formato virtual (lo que ven las aplicaciones):** siempre Float32, a 44,1, 48 o 96 kHz.
- **Formatos físicos** por stream: Float32, SInt32, SInt24 empaquetado (3 bytes) y SInt16, en las tres frecuencias (12 combinaciones). El driver convierte en `ReadInput`/`WriteMix`; el HAL convierte entre virtual y físico.
- **32 bits entero frente a flotante:** el transporte interno es float32. Con SInt32, la resolución efectiva es la mantisa de float, es decir **24 bits**: no es transparente a 32 bits enteros. SInt24 y SInt16 hacen ida y vuelta exacta. Float32 es transparente y no recorta; los formatos enteros recortan a escala completa.

## 4. Pruebas realizadas (en esta sesión, sobre Linux)

| # | Prueba | Resultado |
| --- | --- | --- |
| 1 | `make test`: 102 comprobaciones del núcleo, que cubren reglas anti-lazo, aislamiento con el preset, matriz vacía = silencio, ruta única y suma explícita, cruce prohibido forzado ignorado, datos viejos/huecos/ventana/reset, formatos (ida y vuelta, recorte, −1,0, NaN, SInt16→SInt24 exacto), música estéreo L/R por separado y concurrencia escritor/lector (≈27 000 lecturas sin una muestra corrupta) | **102/102 correctas** |
| 2 | Las mismas pruebas con AddressSanitizer + UBSan (gcc) | Sin errores |
| 3 | Las mismas pruebas con ThreadSanitizer (gcc) | Sin avisos |
| 4 | Compilación del driver contra el SDK de macOS 15.5, arm64 y x86_64, con `-Wall -Wextra -Wpedantic -Werror` | Sin avisos |
| 5 | Enlace del driver como bundle Mach-O con `ld64.lld`; binario universal con `llvm-lipo` | Correcto. Solo exporta `_AVMatrix_Create`; las importaciones son solo de CoreFoundation y libSystem |
| 6 | Compilación y enlace de `avmctl` y de la app (AppKit, ARC) contra el SDK 15.5, ambas arquitecturas | Sin avisos |
| 7 | `Info.plist` del driver: tipo `443ABAB8-…` (kAudioServerPlugInTypeUUID) → fábrica `AVMatrix_Create` | Se analiza correctamente con plistlib |
| 8 | `bash -n` de los tres scripts | Correcto |

Esto demuestra que el código **compila contra las cabeceras reales de Apple** y que **la lógica de matriz, formatos y buffers** es correcta y aislada. **No demuestra** que coreaudiod cargue el plug-in ni que las aplicaciones lo usen.

## 5. Pruebas pendientes (requieren el Mac)

Orden propuesto, que sigue las etapas pedidas:

| Etapa | Prueba | Cómo |
| --- | --- | --- |
| 0 | Registrar el sistema real (versión, arquitectura, CLT, SDK) | `scripts/build.sh` (primeras líneas) |
| 0 | Compilación nativa y pruebas del núcleo en macOS | `scripts/build.sh` |
| 1 | coreaudiod carga el plug-in; aparecen los 3 dispositivos con 8/8, 8/8 y 0/2 canales | `sudo scripts/install.sh`, `avmctl status`, Configuración de Audio MIDI |
| 1 | **Smaart lista «AV Matrix Smaart»** y permite elegirlo como entrada y salida a la vez | Manual en Smaart |
| 1 | Envío y recepción simultáneos en el mismo dispositivo | Smaart: generador → salida 1, matriz `smaart.out.1 → smaart.in.1`; ver la señal en la entrada 1 |
| 2 | Aislamiento muestra a muestra entre los tres dispositivos abiertos a la vez | `avmctl test` |
| 2 | LISTENTO lista «AV Matrix LISTENTO» como entrada y como salida, y puede usar ambas a la vez (una o dos sesiones) | Manual en LISTENTO |
| 2 | Cadena completa con Colombia | Prueba real con el equipo remoto |
| 3 | «AV Matrix Música» se puede elegir como salida del sistema; Apple Music, Spotify, Tidal y DAW llegan a la matriz | Configuración del Sistema → Sonido; `avmctl test` ya cubre la ruta de música |
| 4 | 44,1, 48 y 96 kHz × Float32, SInt32, SInt24, SInt16 | `avmctl test-all` |
| 4 | Recuperación: cerrar y abrir Smaart y LISTENTO en cualquier orden, sin audio viejo ni canales cruzados | Manual + `avmctl test` después |
| 4 | Persistencia de matriz y frecuencia tras reiniciar coreaudiod y el Mac | `avmctl show` antes y después |
| 4 | Retardo de transporte con los buffers reales de Smaart y LISTENTO | `avmctl test` (INFO) |
| — | Estabilidad prolongada (horas) sin cortes; consumo de CPU | Sesión larga de medición |

**No verificado y fuera del control del driver:** la precisión de una medición por internet. Depende de LISTENTO (códec, pérdidas, variación del retardo) y de la red.

## 6. Riesgos y puntos para revisar

1. **Visibilidad en Smaart y LISTENTO:** Smaart y LISTENTO devuelven `DeviceCanBeDefaultDevice = 0` (`AVMatrixDriver.c:805`) para que el sistema no los use por defecto. Lo normal es que las aplicaciones listen todos los dispositivos de todos modos, pero no está probado. Si alguna no los muestra, el cambio es poner `canBeDefaultOutput = true` en `kDevices`.
2. **Firma en macOS 27:** ver §2.
3. **Permiso de micrófono:** la lectura de entradas exige el permiso TCC de micrófono (Smaart, LISTENTO y Terminal para `avmctl test`).
4. **Cambio de frecuencia en caliente:** al cambiar la frecuencia con audio activo, durante un instante puede salir audio grabado a la frecuencia anterior (un fallo audible breve). Se recomienda cambiarla con las aplicaciones cerradas.
5. **Varios clientes en una misma salida:** el HAL suma lo que envíen dos aplicaciones a un mismo dispositivo. Es una propiedad de macOS. Por eso Smaart y LISTENTO no pueden ser la salida por defecto, y la música tiene su propio dispositivo.
6. **IO buffer muy grande:** la ventana válida es de 32 768 frames. Con buffers de IO superiores a unos 8 192 frames por lado no se ha razonado ni probado el margen.
7. **Latencia informada 0:** el driver declara latencia y safety offset 0. El retardo real del bucle virtual se mide con `avmctl test`. Smaart lo compensa con su buscador de delay; no hay delay ajustable.
8. `dist/` contiene binarios de compilación cruzada (lld, SDK 15.5). Se recomienda la compilación nativa con `build.sh`.

## 7. Lista de revisión sugerida para Codex

- [ ] ¿Es aceptable la división Smaart / LISTENTO / Música frente al «único dispositivo»? (§3.1)
- [ ] Propiedades del HAL: comparar `AVM_*_HasProperty`/`GetPropertyData` con el ejemplo NullAudio de Apple
- [ ] Semántica de `ReadInput`/`WriteMix` y formato físico (`AVMatrixDriver.c:1197`)
- [ ] Reloj común y cambio de frecuencia (`AVMatrixDriver.c:440-490`, `1151`)
- [ ] Ventana válida y borrado de huecos (`avm_core.c:251-341`)
- [ ] Regla anti-lazo: ¿hace falta prohibir algo más? (`avm_core.c:61`)
- [ ] Scripts: copia de seguridad antes de reemplazar, reinicio de coreaudiod
- [ ] Ejecutar en el Mac la tabla del §5 y anotar los resultados aquí
