# AV Matrix — informe para revisión de Codex

Fecha: 19 de octubre de 2026 (revisión 2; revisión 1 del 18 de octubre). Proyecto nuevo e independiente en `AVMatrix/`. No modifica ningún archivo existente fuera de esa carpeta. Los scripts de instalación respaldan lo que reemplazan en el Mac.

## 0. Revisión 2 — correcciones pedidas por Codex sobre `9d17362`

Se conservan todas las correcciones de la revisión 1. Antes de modificar se hizo una copia de seguridad del proyecto en `9d17362` (tarball en el directorio temporal de la sesión; el commit también la conserva). **No se instaló nada ni se reinició coreaudiod.**

### 0.A Recuperación de frecuencia: no arrancar audio mientras haya discrepancia

Fallos que reprodujo Codex y su corrección:

| Fallo en `9d17362` | Corrección |
| --- | --- |
| Reversión fallida: `inconsistent=true`, pero StartIO arrancaba | `avm_rate_io_start` devuelve `AVM_RATE_INCONSISTENT` mientras `inconsistent` sea true (`avm_rate.c:152`). El driver responde 'nrdy' en StartIO y lo registra (`AVMatrixDriver.c:1270`) |
| Cambio aplicado en parte y caducado: StartIO arrancaba | Se marca inconsistente (`avm_rate.c:55`) y queda bloqueado por la regla anterior |
| Reversión caducada sin ningún Perform: ni siquiera marcaba `inconsistent` | Toda reversión que caduca sin completarse marca inconsistente. Una reversión solo existe si el cambio previo se aplicó en parte, así que la discrepancia sigue (`avm_rate.c:55`) |
| Pedir la frecuencia actual devolvía SAME sin reparar | En estado inconsistente, pedir la frecuencia actual (o cualquier otra admitida) abre una **recuperación** que se aplica en los tres dispositivos (`avm_rate.c:81`). SAME solo se devuelve si el estado es coherente |

Reglas resultantes:

- Solo un cambio sin ningún Perform conserva el estado previo al caducar o cancelarse.
- `inconsistent` solo vuelve a false cuando **los tres** dispositivos aplican la misma transacción. Las recuperaciones canceladas, caducadas o parciales mantienen el bloqueo.

**Cómo se recupera** (sin audio activo):

- **Propiedad `'rsta'` (nueva, `AVMatrixDriver.c:44`).**
  - Lectura: 0 coherente, 1 cambio en curso, 2 inconsistente (`AVMatrixDriver.c:649`).
  - Escritura: un CFNumber con la frecuencia, 0 = la actual (`AVMatrixDriver.c:1188`), que llama a la misma transacción.
  - Esta vía no depende de que el HAL reenvíe al driver un `NominalSampleRate` sin cambios, cosa que no se pudo verificar sin Mac. Pedir la misma frecuencia por la propiedad estándar también recupera si el HAL la reenvía.
- **`avmctl recover`** (`avmctl.c:165`, `avm_client.c:92`): pide la recuperación y espera a que el estado sea coherente. `avmctl status` muestra el estado de la frecuencia y devuelve error si está inconsistente o en cambio.
- **App:** aviso en rojo y botón **«Reparar frecuencia»**, visible solo en estado inconsistente (`AVMatrixApp.m:325`). Escucha los cambios de `'rsta'`.
- El driver notifica `'rsta'` tras cada Perform, Abort o fallo de petición de frecuencia.

### 0.B `scripts/build.sh` («clang: error: no input files»)

**Causa:** un reemplazo de texto de la revisión 1 coincidió con `"$OUT/test_rate"` dentro de la propia orden `clang` y partió la línea.

**Corrección** (`build.sh:58-60`):

```
clang $CFLAGS -o "$OUT/test_rate" tests/test_rate.c Driver/src/avm_rate.c
"$OUT/test_rate"
bash tests/test_install.sh        # una sola vez
```

**Prueba nueva `tests/test_build_script.sh`:** ejecuta `build.sh` completo con una cadena de herramientas simulada (`tests/fakes/clang`, `xcrun`, `xcode-select`, `sw_vers`, además de `codesign`, `lipo` y `plutil`).

- El `clang` simulado exige fuentes como el real.
- Los binarios de prueba se compilan y ejecutan de verdad con el `cc` del sistema.
- Comprueba: que el script termina bien, que no aparece «no input files», que hay dos resultados de pruebas C (núcleo y frecuencia), que el instalador se prueba **exactamente una vez** y que se generan los tres componentes, con `avm_rate.c` en el driver.
- **Contra el `build.sh` de `9d17362` reproduce el fallo de Codex** (6 de 7 comprobaciones fallan y aparece «clang: error: no input files»).
- `build.sh` acepta `AVM_BUILD_OUT` para no escribir en el repositorio durante la prueba.

### 0.C Pruebas de esta revisión

| Prueba | Resultado |
| --- | --- |
| `tests/test_rate.c`: la prueba que aceptaba arrancar con `inconsistent=true` se sustituyó por una que **exige** el bloqueo. Nuevas: reversión cancelada (StartIO bloqueado en los tres, también tras mucho tiempo, y desbloqueo solo tras recuperar); recuperación pidiendo la misma frecuencia (no SAME, sigue bloqueado hasta el tercer Perform) o pidiendo otra; recuperación cancelada, caducada o parcial (siguen bloqueando); SAME cuando es coherente; cambio parcial caducado; reversión caducada sin Perform; fallo de la petición de reversión | **115/115** |
| Mutaciones que reintroducen los fallos de Codex: StartIO permitido con inconsistencia (42 fallos), caducidad parcial sin marca (11), reversión caducada sin marca (11), SAME sin reparar (36) | Las cuatro se detectan |
| `tests/test_build_script.sh` | **7/7**; con el `build.sh` anterior, 6 fallos |
| `test_core.c` / `test_install.sh` | 115/115 / 57/57 (sin cambios) |
| ASan + UBSan, TSan; compilación cruzada de driver, `avmctl` y app (SDK 15.5, arm64 + x86_64, `-Werror`) | Sin avisos |

### 0.D Pendiente en el Mac (nuevo)

- Si el HAL reenvía o no un `NominalSampleRate` igual al actual. La recuperación no depende de ello, gracias a `'rsta'`.
- Que llamar a `PropertiesChanged` desde `PerformDeviceConfigurationChange` no cause problemas en coreaudiod real. Es el mismo patrón que `SetPropertyData`, pero no está probado.
- Comportamiento de las aplicaciones cuando StartIO devuelve 'nrdy' por inconsistencia: deberían mostrar un error de dispositivo no disponible.

## 0bis. Revisión 1 — correcciones pedidas por Codex sobre `04e6cec`

Codex aceptó la arquitectura Smaart / LISTENTO / Música. Se conservan todos los requisitos. **No se instaló nada ni se reinició coreaudiod** (esta sesión es Linux).

### 0.1 Cambio de frecuencia seguro (`AVMatrixDriver.c:449-501` en `04e6cec`)

Decisión: **se rechaza el cambio mientras haya IO activa** en cualquiera de los tres dispositivos. Además se coordina la transición de los tres como una transacción.

- Nuevo módulo portable `Driver/src/avm_rate.[ch]`, sin bloqueos. El driver lo protege con `gClockMutex`.
- **Inicio** (`avm_rate.c:59`, `AVMatrixDriver.c:556`):
  - se rechaza con `kAudioHardwareNotReadyError` ('nrdy') si hay IO en cualquier dispositivo o ya hay otra transacción;
  - si no, abre una transacción con número de generación (24 bits, codificado en la acción) y la pide a los tres dispositivos.
- **StartIO bloqueado** durante la transacción en los tres dispositivos (`avm_rate.c:131`, `AVMatrixDriver.c:1215`). Así ningún dispositivo arranca a una frecuencia que el host aún no conoce.
- **Perform** (`avm_rate.c:77`, `AVMatrixDriver.c:478`):
  - solo aplica si la generación y la frecuencia coinciden con la transacción vigente;
  - devuelve 'nrdy' y lo trata como cancelación si detecta IO;
  - el reloj ya no se recalcula en caliente: el ancla se crea en el próximo StartIO.
  - La frecuencia se guarda solo cuando los tres dispositivos la aplicaron.
- **Abort, o fallo de `RequestDeviceConfigurationChange`** (`avm_rate.c:96`, `AVMatrixDriver.c:537`, `:556`):
  - si nadie aplicó el cambio, se cierra la transacción;
  - si alguien ya lo aplicó, se abre una **reversión** a la frecuencia anterior en los tres (`AVM_IssueRateRevert`, `AVMatrixDriver.c:461`);
  - si la reversión también se cancela, se marca `inconsistent`, se registra con `os_log_error` y se libera el driver. El siguiente cambio completo lo deja coherente.
  - Las peticiones y cancelaciones de generaciones viejas se ignoran.
- **Caducidad:** una transacción sin Perform ni Abort caduca a los 5 s, para que StartIO no quede bloqueado si el host la abandona.
- **Cambio de formato:** también se comprueba el error de la petición al host, y se valida que el stream pertenezca al dispositivo en Perform.
- **Interfaz:** la app y `avmctl rate` explican el rechazo («detenga el audio en Smaart, LISTENTO y la música»).

### 0.2 Snapshots de matriz con lectura acotada (`avm_core.c:213-232` en `04e6cec`)

- Tres ranuras, cada una con su contador seqlock y los datos como `_Atomic uint64_t` (36 palabras). Hay **cero accesos no atómicos compartidos**, conforme al modelo de memoria de C11.
- **Escritor** (`avm_core.c:200`, `:260`): escribe siempre una ranura **distinta de la publicada** y luego publica el índice con `release`. Un escritor suspendido a mitad no afecta a los lectores.
- **Lector** (`avm_core.c:213`, `:270`): como máximo `AVM_MATRIX_READ_ATTEMPTS` = 4 intentos, sin bucles abiertos ni mutex. Si no obtiene copia coherente, devuelve `false`.
- **Hilo de audio** (`avm_engine_read`): usa la última matriz coherente de ese grupo (`reader_cache`, un lector por grupo) y cuenta `stale_matrix_reads`.
- **Propiedad `'mtrx'` (no tiempo real):** se lee con `gStateMutex`, el mismo que serializa a los escritores, así que la lectura siempre es coherente (`AVMatrixDriver.c:600`).
- **Modelo de memoria también en el audio:** las muestras del buffer pasan a `_Atomic float` con orden relajado. En arm64 y x86_64 compilan a cargas y almacenamientos normales.
- **Hallazgo durante la revisión:** la prueba de concurrencia existente falló en un build sin sanitizers (1 275 muestras erróneas). La causa: un lector desalojado mientras el escritor, sin ritmo de tiempo real, daba una vuelta completa al buffer (65 536 frames). Se añadió `writing_end`, publicado **antes** de escribir (`avm_core.c:306`), y una validación posterior en el lector con barrera `acquire` (`avm_core.c:394`). Si el escritor pudo sobrescribir lo leído, el bloque se entrega como silencio y se cuenta en `overrun_reads`. En tiempo real esto exigiría un lector retrasado más de 0,68 s a 96 kHz; ahora es imposible entregar audio de otra vuelta.

### 0.3 Firmas en `install.sh` (`scripts/install.sh:49-60` en `04e6cec`)

- Ya no se re-firma por defecto.
- Sin `AVM_SIGN_IDENTITY` ni `--sign-identity`, se **exige** una firma válida.
- Con `-`, solo se firma ad hoc lo que no tiene firma válida. **Una firma con identidad (Developer ID) se conserva**, y se comprueba en staging que no cambió.
- Con una identidad concreta, se firma con ella lo que no esté ya firmado por ella, y se comprueba que el resultado no es ad hoc.
- La firma se hace **sobre la copia de staging**, nunca sobre el origen.
- Cualquier error de `codesign` detiene la instalación. Se verifica con `codesign --verify --strict [--deep]` el origen, el staging y el destino final.
- `build.sh` verifica las firmas de los tres componentes y muestra la autoridad.

### 0.4 Instalación transaccional (`scripts/install.sh:13-60` en `04e6cec`)

1. **Validación de los tres componentes** antes de tocar nada (`install.sh:174`): Info.plist (`plutil -lint`), identificador de bundle, tipo `443ABAB8-…` y fábrica `AVMatrix_Create` en el driver, ejecutable presente y ejecutable, arquitectura del Mac (`lipo -archs`) y firma.
2. **Staging** junto a cada destino, en el mismo volumen (`install.sh:214`). Se quita la cuarentena solo si existe, y se aplican propietario, permisos y firma.
3. **Respaldo** verificado con `diff -rq` (`install.sh:256`).
4. **Sustitución** por `mv`, guardando lo anterior como `.old` (`install.sh:269`).
5. **Verificación final** (`install.sh:286`): firmas y plist en el destino. Luego reinicia coreaudiod y espera `avmctl status` (20 s por defecto). Si no queda operativa, **restaura** y termina con error.

- Una trampa `EXIT` restaura ante **cualquier** fallo a partir del paso 4. Si ya había reiniciado coreaudiod, lo reinicia de nuevo con la versión anterior. Siempre limpia el staging.
- Opciones: `--dry-run` (valida y prepara sin tocar nada), `--no-restart` (instala y verifica archivos; deja la comprobación de funcionamiento como PENDIENTE) y `--sign-identity=`.
- Las rutas y herramientas son configurables por variables `AVM_*`, `CODESIGN`, etc., solo para poder probarlo fuera de `/Library`.

### 0.5 Pruebas nuevas de esta revisión

| Archivo | Qué cubre | Resultado |
| --- | --- | --- |
| `tests/test_rate.c` | 48 comprobaciones: rechazo con IO, transacción completa, StartIO bloqueado durante el cambio, Perform con IO, Abort antes y después de aplicar, reversión, reversión cancelada (inconsistente), generaciones viejas, caducidad, vuelta del contador | **48/48** |
| `tests/test_core.c` (nuevas) | Escritor suspendido en ranura libre (100 000 lecturas sin efecto); ranura publicada inestable: lectura `false` acotada, destino intacto, audio con la última matriz y contador; concurrencia escritor/2 lectores sin copias mezcladas; vuelta del escritor detectada como silencio | **115/115** en total |
| `tests/test_install.sh` | 57 comprobaciones con `codesign`/`plutil`/`lipo`/`xattr` simulados: instalación nueva, actualización, firma inválida (nada cambia), Developer ID conservada con `-`, ad hoc solo en staging, identidad elegida, error de firma, comprobación de funcionamiento fallida (restaura exacto y reinicia de nuevo), fallo a mitad de sustitución (con y sin versión previa), plist, arquitectura, ejecutable o fábrica inválidos, prueba en seco, `--sign-identity` | **57/57** |
| Mutaciones | Se desactivó la restauración (7 pruebas fallan) y se forzó la firma ad hoc sobre Developer ID (2 pruebas fallan) | Las pruebas detectan ambas |
| Concurrencia bajo carga | 24 ejecuciones paralelas en 4 núcleos: 16-22 vueltas del escritor detectadas por ejecución, 0 muestras erróneas | Correcto |
| Sanitizers | ASan + UBSan (núcleo y frecuencia) y TSan (núcleo) | Sin avisos |
| Compilación cruzada | Driver, `avmctl` y app contra el SDK 15.5, arm64 + x86_64, `-Wall -Wextra -Wpedantic -Werror` | Sin avisos; el driver solo exporta `_AVMatrix_Create` |

**Límite de estas pruebas:** el instalador se probó con **bash 5 en Linux**. macOS usa bash 3.2 en `/bin/bash`; el script evita arrays vacíos con `set -u` y otras construcciones de bash 4, pero no se pudo ejecutar con bash 3.2 aquí (no hay acceso a sus fuentes). `build.sh` ejecuta `tests/test_install.sh` en el Mac con el bash del sistema.

## 1. Resumen

AudioServerPlugIn de CoreAudio, escrito en C, que publica tres dispositivos virtuales con un reloj común y una matriz de enrutamiento. Incluye:

- una app de matriz (Objective-C/AppKit);
- una herramienta de Terminal con autoprueba (`avmctl`);
- scripts de compilación, instalación y desinstalación.

**Estado honesto:** el código compila y enlaza para macOS (arm64 y x86_64) y pasan las pruebas del núcleo, de la transacción de frecuencia y del instalador. **No se ha ejecutado en ningún Mac.** No hay ninguna prueba hecha con Smaart, LISTENTO, Apple Music ni coreaudiod.

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

### 3.1 Dispositivos (`Driver/src/AVMatrixDriver.c:75`)

| Dispositivo | UID | Entradas | Salidas | Por defecto del sistema |
| --- | --- | --- | --- | --- |
| AV Matrix Smaart | `AVMatrix_Smaart_UID` | 8 «Entrada N» | 8 «Salida N» | No |
| AV Matrix LISTENTO | `AVMatrix_Listento_UID` | 8 «Envío N» | 8 «Retorno N» | No |
| AV Matrix Música | `AVMatrix_Music_UID` | — | 2 «Música L/R» | Sí, como salida; no como dispositivo de alertas |

- Terminal de stream: `kAudioStreamTerminalTypeLine` en las dos direcciones (`AVMatrixDriver.c:1022`). Las entradas no se declaran como micrófono.
- Transporte `kAudioDeviceTransportTypeVirtual`. Mismo `ClockDomain` en los tres.

**Desviación consciente sobre el «único dispositivo»:** Smaart ve un único dispositivo (AV Matrix Smaart) y lo usa para enviar y recibir. LISTENTO usa un segundo dispositivo. Si LISTENTO usara el de Smaart, el HAL de macOS sumaría las salidas de ambos clientes (ruido rosa + retorno) en los mismos canales antes de que el driver las reciba. Es justo la mezcla involuntaria que el pedido prohíbe. **Codex: confirmar que esta lectura del requisito es aceptable para Antonio.**

### 3.2 Matriz (`Driver/src/avm_core.[ch]`)

- 18 fuentes: Smaart salida 1-8, LISTENTO retorno 1-8, Música L/R.
- 16 destinos: Smaart entrada 1-8, LISTENTO envío 1-8.
- Cruces binarios a ganancia unidad. Varias fuentes en un destino se suman en float.
- **Regla anti-lazo** (`avm_core.c:61`): prohibido LISTENTO retorno → LISTENTO envío. Se aplica en tres sitios:
  - el driver rechaza la matriz completa (`AVMatrixDriver.c:610`, `avm_matrix_is_valid`);
  - el motor ignora el cruce aunque llegue a la matriz (`avm_engine_read`);
  - la UI deshabilita la casilla.
- Permitido a propósito: Smaart salida → Smaart entrada (referencia en bucle local). No es realimentación, porque Smaart no reenvía su entrada a su salida.
- Preset predeterminado: Smaart salida N → LISTENTO envío N; LISTENTO retorno N → Smaart entrada N. La música queda sin conectar.
- Se guarda con `WriteToStorage` (claves `matrix`, `sample_rate`, `format_stream_N`) y se valida al cargar.
- Control desde fuera: propiedad personalizada `'mtrx'` (CFData de 16×18 bytes, fila = destino), publicada en el plug-in y en los tres dispositivos (`kAudioObjectPropertyCustomPropertyInfoList`).

### 3.3 Transporte y reloj

- Un buffer circular float32 de 65 536 frames por fuente. Ventana válida: los 32 768 frames más recientes escritos de cada grupo de fuentes.
- `WriteMix` escribe en la posición `mOutputTime.mSampleTime`; `ReadInput` lee en `mInputTime.mSampleTime` y suma según la matriz.
- **Sin datos viejos** (`avm_core.c:291-408`):
  - lo que está fuera de `[max(written_end - ventana, valid_begin), written_end)` se lee como silencio;
  - si una aplicación deja de enviar y vuelve, el hueco se borra al escribir;
  - cuando el último cliente de los tres dispositivos se detiene y otro arranca, `StartIO` vacía el motor y crea una nueva línea de tiempo (`AVMatrixDriver.c:1206`).
- **Reloj común** (`AVMatrixDriver.c:1244`): `GetZeroTimeStamp` usa una sola ancla para los tres dispositivos (periodo 16 384 frames). Por eso la posición en frames coincide entre Smaart, LISTENTO y Música.
- **Cambio de frecuencia:** transacción sobre los tres dispositivos, solo sin IO activa (ver §0.1).
- **Matriz:** snapshots en tres ranuras atómicas con lectura acotada (ver §0.2).
- El hilo de IO no reserva memoria ni toma mutex, salvo el mutex corto de `GetZeroTimeStamp`, que es el mismo patrón que el ejemplo NullAudio de Apple.

### 3.4 Formatos

- **Formato virtual (lo que ven las aplicaciones):** siempre Float32, a 44,1, 48 o 96 kHz.
- **Formatos físicos** por stream: Float32, SInt32, SInt24 empaquetado (3 bytes) y SInt16, en las tres frecuencias (12 combinaciones). El driver convierte en `ReadInput`/`WriteMix`; el HAL convierte entre virtual y físico.
- **32 bits entero frente a flotante:** el transporte interno es float32. Con SInt32, la resolución efectiva es la mantisa de float, es decir **24 bits**: no es transparente a 32 bits enteros. SInt24 y SInt16 hacen ida y vuelta exacta. Float32 es transparente y no recorta; los formatos enteros recortan a escala completa.

## 4. Pruebas realizadas (en esta sesión, sobre Linux)

| # | Prueba | Resultado |
| --- | --- | --- |
| 1 | `make test` (revisión inicial): 102 comprobaciones del núcleo; hoy son 115 + 48 de frecuencia + 57 del instalador (§0.5), que cubren reglas anti-lazo, aislamiento con el preset, matriz vacía = silencio, ruta única y suma explícita, cruce prohibido forzado ignorado, datos viejos/huecos/ventana/reset, formatos (ida y vuelta, recorte, −1,0, NaN, SInt16→SInt24 exacto), música estéreo L/R por separado y concurrencia escritor/lector (≈27 000 lecturas sin una muestra corrupta) | **102/102 correctas** |
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
| 4 | Cambio de frecuencia rechazado con audio activo (desde Smaart, la app y `avmctl`) y admitido sin él; comprobar en el registro que no hay transacciones abandonadas ni `inconsistent` | Manual + `log show … \| grep -i avmatrix` |
| 0 | Pruebas del instalador con el bash 3.2 de macOS | `bash tests/test_install.sh` (lo hace `build.sh`) |
| 1 | Instalación real: `--dry-run`, luego instalación con reinicio; restauración si falla | `sudo scripts/install.sh --dry-run`, `sudo scripts/install.sh` |
| 4 | Recuperación: cerrar y abrir Smaart y LISTENTO en cualquier orden, sin audio viejo ni canales cruzados | Manual + `avmctl test` después |
| 4 | Persistencia de matriz y frecuencia tras reiniciar coreaudiod y el Mac | `avmctl show` antes y después |
| 4 | Retardo de transporte con los buffers reales de Smaart y LISTENTO | `avmctl test` (INFO) |
| — | Estabilidad prolongada (horas) sin cortes; consumo de CPU | Sesión larga de medición |

**No verificado y fuera del control del driver:** la precisión de una medición por internet. Depende de LISTENTO (códec, pérdidas, variación del retardo) y de la red.

## 6. Riesgos y puntos para revisar

1. **Visibilidad en Smaart y LISTENTO:** Smaart y LISTENTO devuelven `DeviceCanBeDefaultDevice = 0` (`AVMatrixDriver.c:895`) para que el sistema no los use por defecto. Lo normal es que las aplicaciones listen todos los dispositivos de todos modos, pero no está probado. Si alguna no los muestra, el cambio es poner `canBeDefaultOutput = true` en `kDevices`.
2. **Firma en macOS 27:** ver §2.
3. **Permiso de micrófono:** la lectura de entradas exige el permiso TCC de micrófono (Smaart, LISTENTO y Terminal para `avmctl test`).
4. **Cambio de frecuencia:** ahora se rechaza con audio activo (§0.1). Riesgo nuevo, por verificar en el Mac: si Smaart cambia la frecuencia sin detener antes su propio audio, verá el error 'nrdy' y el usuario tendrá que detener el audio y repetir. Si macOS mantiene abierta la salida del sistema (AV Matrix Música como salida por defecto), puede bastar con pausar la música; si no, hay que elegir otra salida del sistema durante el cambio.
5. **Varios clientes en una misma salida:** el HAL suma lo que envíen dos aplicaciones a un mismo dispositivo. Es una propiedad de macOS. Por eso Smaart y LISTENTO no pueden ser la salida por defecto, y la música tiene su propio dispositivo.
6. **IO buffer muy grande:** la ventana válida es de 32 768 frames. Con buffers de IO superiores a unos 8 192 frames por lado no se ha razonado ni probado el margen.
7. **Latencia informada 0:** el driver declara latencia y safety offset 0. El retardo real del bucle virtual se mide con `avmctl test`. Smaart lo compensa con su buscador de delay; no hay delay ajustable.
8. `dist/` contiene binarios de compilación cruzada (lld, SDK 15.5) con solo la firma del enlazador. El instalador los rechaza salvo `--sign-identity=-`. Se recomienda la compilación nativa con `build.sh`.
9. **Comportamiento real del host ante la transacción:** el orden de Perform/Abort de coreaudiod con tres dispositivos y la llamada síncrona o diferida a Perform están razonados según la documentación de `AudioServerPlugIn.h`, pero no probados.

## 7. Lista de revisión sugerida para Codex

- [ ] ¿Es aceptable la división Smaart / LISTENTO / Música frente al «único dispositivo»? (§3.1)
- [ ] Propiedades del HAL: comparar `AVM_*_HasProperty`/`GetPropertyData` con el ejemplo NullAudio de Apple
- [ ] Semántica de `ReadInput`/`WriteMix` y formato físico (`AVMatrixDriver.c:1290`)
- [ ] Transacción y recuperación de frecuencia (`avm_rate.c`, `AVMatrixDriver.c:464-600`, `:649`, `:1188`, `:1270`)
- [ ] Snapshots de matriz y validación de vuelta del escritor (`avm_core.c:200-280`, `:306`, `:394`)
- [ ] Instalador transaccional y firmas (`scripts/install.sh`, `tests/test_install.sh`)
- [ ] Regla anti-lazo: ¿hace falta prohibir algo más? (`avm_core.c:61`)
- [ ] Scripts: copia de seguridad antes de reemplazar, reinicio de coreaudiod
- [ ] Ejecutar en el Mac la tabla del §5 y anotar los resultados aquí
