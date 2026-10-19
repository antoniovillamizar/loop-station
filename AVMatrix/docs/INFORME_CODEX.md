# AV Matrix — informe para revisión de Codex

Fecha: 19 de octubre de 2026 (revisiones 2 a 13; revisión 1 del 18 de octubre). Proyecto nuevo e independiente en `AVMatrix/`. No modifica ningún archivo existente fuera de esa carpeta. Los scripts de instalación respaldan lo que reemplazan en el Mac.

## 0. Revisión 13 — mixer: arranque con MUTE, guardado ordenado y veredicto de la autoprueba (sobre `ee604ad`)

Codex revisó `ee604ad` y encontró tres problemas. Antes de modificar se hizo una copia de seguridad del proyecto en `ee604ad`. **No se instaló nada ni se reinició coreaudiod.** Cambian el motor, el driver y `avmctl`: hay que instalar con `install.sh` completo, no con `--app-only`.

**Qué se comprobó y dónde:**
- **En Linux:** todas las pruebas nuevas, con ASan, UBSan y TSan; las mutaciones; la comparación bit a bit a 0 dB; la compilación cruzada.
- **En un Mac:** nada todavía.

### 0.13.1 Un canal ya silenciado dejaba pasar audio al arrancar

- **Causa (confirmada):** las rampas solo se ajustaban al objetivo en el arranque global (`avm_engine_reset`, primer cliente). Si Smaart ya estaba activo y Música arrancaba en MUTE, Música hacía una rampa desde el último nivel aplicado. La mutación que reproduce el código anterior da exactamente la primera muestra de Codex: **0,998046875**.
- **Corrección** (`avm_core.c`, `avm_group_begin`, `:593`): al empezar cada bloque, el hilo de IO dueño del grupo pone sus canales directamente en el volumen y el MUTE vigentes, **sin rampa**, si el audio del grupo empieza o se reanuda. Eso ocurre en tres casos:
  - **primer bloque:** el grupo nunca había procesado audio;
  - **bloque no contiguo:** el instante no sigue al del bloque anterior (una pausa, un stream inactivo, una reanudación);
  - **arranque pedido por el driver:** `AVM_StartIO` lo pide (`avm_engine_request_settle_src/dst`, `:577`) cuando **ese** dispositivo pasa de parado a activo (`gRate.running[dev] == 1`, `AVMatrixDriver.c:1404`). Es un almacenamiento atómico que el hilo de IO consume con un intercambio atómico.
- **Los demás dispositivos no se reinician.** Con el audio en marcha y contiguo, los cambios siguen usando la rampa de 512 muestras.
- Se aplica igual a los destinos: LISTENTO, al empezar a leer con un Envío ya en MUTE o atenuado, lo respeta desde la primera muestra.
- A 0 dB sin mute la salida sigue **idéntica bit a bit a `fddbc62`** (`tests/compare_engine_unity.sh`).

### 0.13.2 Los ajustes podían guardarse fuera de orden

- **Causa:** `AVM_SetMixerProperty` aplicaba bajo `gStateMutex` y guardaba fuera. Con A y B simultáneos se podía aplicar A, B y guardar B, A. **`AVM_SetMatrixProperty` tenía el mismo problema** y también se corrige.
- **Corrección:** módulo nuevo `Driver/src/avm_persist.[ch]`, portable y probado en Linux.
  - Cada cambio se aplica bajo `gStateMutex` y recibe una generación.
  - Después, bajo un mutex propio de guardado, se vuelve a leer el **estado vigente** bajo `gStateMutex` y se guarda ese, solo si es más nuevo que el último guardado.
  - Así **el último guardado siempre contiene el último ajuste aplicado**, sea cual sea el orden en que terminen los guardados.
  - El almacenamiento, que es lento, nunca se escribe con `gStateMutex` tomado, y el hilo de audio no toma ninguno de los dos mutex.
- Lo usan la matriz y el mixer (`AVM_SetMixerProperty`, `:743`).

### 0.13.3 La autoprueba no fallaba si no podía restaurar

- **Corrección:** `Client/avm_selftest.h` (nuevo) decide el veredicto. `avmctl test` registra cualquier fallo al restaurar **la frecuencia, los formatos, el mixer o la matriz** (`Client/avmctl.c:773`). En ese caso termina con «RESULTADO: HAY FALLOS (no se pudo restaurar el estado anterior)» y **código 1**, aunque el audio haya pasado.
- **Fase nueva 2d en `avmctl test`** (`:691`, `run_devices_staggered`, `:471`): reproduce en el Mac el caso de Codex.
  - Smaart arranca, y con Smaart ya en marcha se aplica el mixer: Música L en MUTE, Música R a −6 dB, Envío 1 en MUTE y Envío 2 a −6 dB.
  - Después arranca Música y, al final, LISTENTO, sin detener a los anteriores.
  - Desde la primera muestra solo se admiten 0 (antes de que llegue la fuente) o el valor exacto; **cualquier valor intermedio es un fallo**.

### 0.13.4 Pruebas

| Prueba | Dónde | Qué comprueba |
| --- | --- | --- |
| `tests/test_mixer.c` (+14, 99) | Linux, ASan, UBSan, TSan | **Caso de Codex:** Smaart activo, Música en MUTE y a −6 dB antes de abrir su audio: silencio y 0,5 desde la primera muestra, y Smaart sin cambios. **Reanudación** de Música tras una pausa con ajustes nuevos. **Destino LISTENTO** ya en MUTE o atenuado al empezar a leer, y lectura reanudada. **Arranque pedido** por StartIO con un bloque contiguo. Sin petición y contiguo, la rampa se mantiene; la petición es por grupo. Con el audio en marcha, el MUTE sigue con rampa. **Mutación** (quitar el ajuste al arrancar): 10 fallos, con la primera muestra 0,998047, como en la revisión de Codex |
| `tests/test_persist.c` (nueva, 13) | Linux, ASan, UBSan, TSan | Almacenamiento simulado **deliberadamente desordenado**. **Caso de Codex:** el método anterior reproduce MUTE vigente 1 y guardado 0; el nuevo guarda 1. **Retraso entre aplicar y guardar.** **A guarda antes que B** pero después de que B se aplique: se guarda el estado vigente (B). **8 hilos × 60 cambios** de mixer y matriz con guardados aleatorios: lo guardado es lo vigente y la última generación aplicada es la guardada. Un ajuste rechazado no escribe. **Mutaciones:** guardar el valor pedido en vez del vigente (1 fallo) y quitar el mutex de guardado (3 fallos) |
| `tests/test_selftest.c` (nueva, 8) | Linux | Las 32 combinaciones: código 0 y «CORRECTO» solo si todo pasó y todo se restauró. Falla restaurar el mixer, la matriz, la frecuencia o un formato: código 1 |
| `tests/test_avmctl_static.sh` (nueva, 8) | Linux | `avmctl` registra cada fallo al restaurar y sale con el veredicto. La versión `ee604ad` da 7 fallos; quitar el registro del mixer da 1 |
| `tests/test_build_script.sh` (+4, 31) | Linux | El driver incluye `avm_persist.c`; `build.sh` ejecuta las pruebas nuevas |

**Resultados (Linux):**

| Suite | Resultado |
| --- | --- |
| Núcleo | 122/122 |
| Frecuencia | 115/115 |
| Medidores | 117/117 |
| Nombres | 51/51 |
| Manual | 328/328 |
| **Mixer** | **99/99** |
| Faders | 17/17 |
| **Motor a 0 dB** | **idéntico a `fddbc62`** |
| **Guardado** | **13/13** |
| **Veredicto** | **8/8** |
| **avmctl** | **8/8** |
| Instalador | 102/102 |
| `build.sh` | 31/31 |
| Textos | 36/36 |

Sanitizers sin avisos. Compilación cruzada de driver, app, `avmctl` y pruebas nativas sin avisos.

**Pendiente en el Mac:**
- `avmctl test` y `test-all`: las cuatro líneas «arranque:» de la fase 2d y la fase 2c deben salir OK.
- Repetir a mano el caso de Codex (Música en MUTE con Smaart activo, y después abrir Música).
- Persistencia tras reiniciar.

## 0septies-bis. Revisión 12 — mixer: volumen (solo atenuación) y MUTE por canal (sobre `fddbc62`)

Encargo: una ventana «Mixer» con volumen y MUTE para las 18 entradas a la matriz y las 16 salidas. Antes de modificar se hizo una copia de seguridad del proyecto en `fddbc62`. **No se instaló nada ni se reinició coreaudiod.**

**Esta revisión cambia el motor y el driver:**
- `avm_core.[ch]`;
- `AVMatrixDriver.c`, con la propiedad nueva `'mixr'`;
- `avm_client.[ch]` y `avmctl`.

Hay que instalarla con `install.sh` completo, **no con `--app-only`**.

**Qué se comprobó y dónde:**
- **En Linux:**
  - el motor del mixer (`tests/test_mixer.c`), con ASan, UBSan y TSan;
  - la escala de los faders (`tests/test_fader.c`);
  - la **comparación bit a bit con el motor de `fddbc62`** (`tests/compare_engine_unity.sh`);
  - el manual;
  - las comprobaciones estáticas;
  - la compilación cruzada de driver, app, `avmctl` y las tres pruebas nativas;
  - la maqueta de la ventana.
- **En un Mac:** nada todavía. No se han ejecutado el driver con `'mixr'`, `avmctl test` con la fase nueva del mixer ni `tests/test_mixer_ui.m`.

### 0.12.1 Motor (`Driver/src/avm_core.[ch]`)

- **Ajustes** (`avm_mixer`): ganancia lineal en [0, 1] y mute 0/1 por canal. `avm_mixer_is_valid` **rechaza cualquier ganancia mayor que 1** (no se puede amplificar), además de NaN, infinito, negativos y mutes distintos de 0/1. Al activar el MUTE la ganancia se conserva.
- **Dónde actúa:**
  - **Fuentes:** en `avm_engine_write_metered` (`:662`), antes de guardar la muestra en el buffer, es decir, antes de la matriz;
  - **Destinos:** en `avm_engine_read_metered` (`:748`), después de la suma y antes de convertir al formato.
  - Los medidores observan esos mismos valores: muestran **el nivel resultante**.
- **Sin chasquidos ni bloqueos:**
  - `avm_engine_set_mixer` (`:567`) solo publica la ganancia objetivo de cada canal con un almacenamiento atómico relajado;
  - al principio de cada bloque, el hilo de IO dueño del canal lee el objetivo con una carga atómica (`avm_gain_begin_block`, `:451`) y, si cambió, hace una **rampa lineal de 512 muestras** (unos 10,7 ms a 48 kHz) que avanza muestra a muestra;
  - el dueño es el único escritor de su grupo de fuentes o el único lector de su grupo de destinos, el mismo criterio que `reader_cache`;
  - la rampa nunca pasa del objetivo y termina exactamente en él;
  - no hay mutex, esperas ni retardo de transporte: el impulso sale en el mismo instante.
- **0 dB idéntico:** a ganancia 1 la muestra no se toca; a 0 se entrega +0 exacto (`avm_gain_apply`, `:476`).
- **Reinicio:** `avm_engine_reset` termina las rampas (sin IO activo).
- **Serialización:** `avm_mixer_pack` y `avm_mixer_unpack` (cabecera `AVX1`, 8 bytes por canal, el relleno debe ser 0, se valida al desempaquetar).

### 0.12.2 Driver (`Driver/src/AVMatrixDriver.c`)

- Propiedad personalizada **`'mixr'`** (CFData) en el plug-in y en los tres dispositivos, como `'mtrx'`. Se declara en `kCustomProps`, en Has, Size, Get, IsSettable y Set.
- Al escribirla (`AVM_SetMixerProperty`, `:726`):
  - se desempaqueta y valida, y se rechaza entera si algo no es válido;
  - se aplica con `gStateMutex` tomado (solo el hilo de control; el de audio no lo toma);
  - se guarda en el almacenamiento con la clave `"mixer"` (`:333`) y se notifica a todos los dispositivos.
- Al cargar el driver (`:371`), si no hay ajustes guardados (**instalación nueva**) queda todo a 0 dB y sin mute. Un ajuste guardado inválido se descarta con un aviso en el registro.

### 0.12.3 Cliente y Terminal

- `avm_get_mixer` y `avm_set_mixer` en `Client/avm_client.[ch]`.
- `avmctl mixer`, `mixer reset`, `mixer gain <canal> <dB|-inf>` (rechaza dB positivos) y `mixer mute <canal> on|off` (`Client/avmctl.c:703`).
- **`avmctl test` y `test-all`:**
  - guardan y restauran el mixer;
  - ponen 0 dB sin mute en las fases anteriores;
  - añaden la **fase 2c** (`:586`): comprueba muestra por muestra la fuente a −6 dB, el destino en MUTE, la suma con el destino a −6 dB, la fuente en MUTE, −6 −6 dB y el aislamiento a 0 dB, con valores k/64 exactos en los cuatro formatos;
  - comprueba los medidores tras el mixer y que el driver rechaza amplificar.

### 0.12.4 App (`App/AVMatrixApp.m`, `App/avm_fader.[ch]`)

- **Botón «Mixer»** en la barra superior (`showMixer:`, `:2042`). La ventana (`AVMMixerController`, `:1328`) es redimensionable y tiene desplazamiento.
- **Dos secciones:**
  - **ENTRADAS A LA MATRIZ:** Smaart Salidas 1-8, LISTENTO Retornos 1-8 y Música L/R;
  - **SALIDAS DE LA MATRIZ:** Smaart Entradas 1-8 y LISTENTO Envíos 1-8.
- **Cada tira:** número, nombre personalizado (con la descripción completa en la ayuda emergente), medidor vertical con LED de saturación y pico en dBFS, fader vertical con lectura en dB y botón **MUTE**, que se pone rojo cuando está activo.
- **Fader:** escala cúbica (ganancia = posición³), arriba 0 dB exactos (se ajusta a 0 dB por encima del 99,8 %) y abajo silencio. Doble clic u Opción+clic vuelve a 0 dB. El recorrido termina en 0 dB: no amplifica.
- **«Silenciar todos»** activa los 34 MUTE sin perder los volúmenes. **«Restaurar 0 dB y quitar mutes…»** pide confirmación, porque el audio puede subir de golpe.
- **Envíos al driver:** se agrupan cada 30 ms (`scheduleSend`, `:1519`); el MUTE se envía al momento. El temporizador y los medidores corren en `NSRunLoopCommonModes`, así siguen funcionando mientras se arrastra un fader.
- **Coherencia con otros cambios:** la app escucha `'mixr'`, así que un cambio hecho con `avmctl` se refleja, sin pisar un cambio local pendiente.
- La línea de estado principal avisa en naranja si hay canales atenuados o en MUTE (`:1875`).
- **Se conservan:** conexiones, nombres, icono, manual y pantalla compacta.

### 0.12.5 Manual

- Nueva sección **«Mixer: volumen y MUTE de cada canal»**, con un apartado **«Cómo afecta a la medición en Smaart»**:
  - bajar Salida 1 baja a la vez el envío y la referencia: la curva no cambia, pero empeora la relación señal/ruido;
  - bajar la referencia (Entrada 8) o la medición (Entrada 1), o Envío 1 o Retorno 1, desplaza la curva de magnitud exactamente esos dB;
  - un MUTE en la referencia o en la medición deja sin buscador de delay ni coherencia;
  - la fase y el retardo no cambian.
- **También se actualizan:** inicio, matriz, referencia en Smaart, medidores (nivel tras el mixer), diagnóstico (canal en MUTE o atenuado) y limitaciones (solo atenúa; rampa de unos 10 ms).

### 0.12.6 Pruebas

| Prueba | Dónde | Qué comprueba |
| --- | --- | --- |
| `tests/test_mixer.c` (nueva, 85) | Linux, también con ASan, UBSan y TSan | Instalación nueva a 0 dB sin mute. Rechazo de >1, negativos, NaN, ±inf y mute 2, sin cambiar nada. dB ↔ ganancia. Serialización (ida y vuelta, cabecera, relleno, valores inválidos). **0 dB bit a bit** tras pasar por otros ajustes, en los 4 formatos (−0, subnormales, ±1, >1). **Niveles:** fuente 0,5 → mitad exacta. **Aislamiento:** los demás canales exactos. **Sumas:** volumen de salida después de la suma ((a+b)·0,25) y de entrada antes (a·0,5+b). **Medidores** con el nivel resultante. **MUTE:** silencio +0 exacto, conserva el volumen y quitarlo recupera el nivel; un NaN o inf de una fuente en MUTE no contamina la suma; «silenciar todos» y «restaurar». **Rampas:** ningún salto mayor que 1/512 entre muestras, bajada y subida progresivas, silencio exacto al final, cambio a mitad de rampa sin saltos. **Nunca amplifica** (|salida| ≤ |entrada| con volúmenes aleatorios). **Sin retardo** (el impulso sale en el mismo instante). Saturación tras el volumen. **Concurrencia:** el control cambia el mixer sin parar durante el audio |
| Mutaciones del motor | Linux | Sin rampa (9 fallos), sin volumen de entrada (15), sin volumen de salida (8), permitir amplificar (6), sin silencio exacto (1), mute de fuente ignorado (6): todas detectadas |
| `tests/compare_engine_unity.sh` + `tests/engine_dump.c` (nuevas) | Linux | **Mismo resultado que el motor de `fddbc62` sin mixer**, bit a bit: 12,5 MB de audio de los 16 destinos y medidores, con matrices aleatorias, lecturas desplazadas y valores límite en los 4 formatos. Se ejecuta en `make test` |
| `tests/test_fader.c` (nueva, 17) | Linux | 0 dB exactos arriba, silencio abajo, monótona, nunca por encima de 1, siempre válida para el driver, escala útil (−6 dB al 79 %), ida y vuelta, lecturas «0.0 dB», «-6.0 dB», «−∞» y sin «-0.0» |
| `tests/test_mixer_ui.m` (nueva, nativa, 38 comprobaciones; modo `offline`, no toca el driver) | Mac (sin ejecutar) | Dos secciones y cinco grupos; 34 tiras completas (número, nombre, medidor, fader a 0 dB con lectura y MUTE); nombre personalizado con tildes; fader a la mitad = 0,125 «-18.1 dB» sin tocar los demás canales; 50 movimientos agrupados en 1-3 envíos; abajo «−∞», arriba 0 dB exactos, sin amplificación; MUTE conserva el volumen, se envía al momento y se resalta; «Silenciar todos» y «Restaurar»; medidores, saturación y «—»; botón «Mixer» en la ventana principal |
| `tests/test_manual.c` (+18, 328) | Linux | Los temas del mixer y su efecto en Smaart; la referencia remite al mixer; ya no dice que no hay control de nivel |
| `tests/test_app_strings.sh` (+8, 36) y `tests/test_build_script.sh` (+4, 27) | Linux | Botones Mixer, Silenciar todos, Restaurar y MUTE; fader hasta 0 dB (mutación detectada); temporizadores en modos comunes; confirmación al restaurar; escucha de `'mixr'`. `build.sh` compila y ejecuta `test_mixer`, `test_fader` y `test_mixer_ui`, e incluye `avm_fader.c` en la app |

**Resultados (Linux):**

| Suite | Resultado |
| --- | --- |
| Núcleo | 122/122 |
| Frecuencia | 115/115 |
| Medidores | 117/117 |
| Nombres | 51/51 |
| Manual | 328/328 |
| **Mixer** | **85/85** |
| **Faders** | **17/17** |
| **Motor a 0 dB** | **idéntico a `fddbc62`** |
| Instalador | 102/102 |
| `build.sh` | 27/27 |
| Textos | 36/36 |

ASan, UBSan y TSan sin avisos (también `test_core` y `test_meters` con el mixer dentro del motor). Compilación cruzada de driver, app, `avmctl` y pruebas nativas sin avisos con `-Werror`.

**Para revisar con atención:**
- **Rampa por hilo dueño:** supone, como `reader_cache`, un solo lector por grupo de destinos y un solo escritor por grupo de fuentes. Si coreaudiod llamara a `ReadInput` del mismo dispositivo desde dos hilos a la vez, habría una carrera en el estado de la rampa (no en el audio del buffer).
- **Medidores de la ventana principal:** los de los transmisores muestran ahora el nivel **después** del volumen de entrada (antes de la matriz).
- **Almacenamiento:** se escribe en cada envío, como mucho cada 30 ms mientras se arrastra un fader.

**Pendiente en el Mac** (lista en `INSTALAR.md`):
```bash
bash scripts/build.sh                 # incluye test_mixer, test_fader y test_mixer_ui
sudo bash scripts/install.sh --dry-run
sudo bash scripts/install.sh          # instalación COMPLETA (no --app-only): reinicia coreaudiod
avmctl mixer                          # instalación nueva: todo a 0 dB sin mute
avmctl test && avmctl test-all        # fase 2c del mixer en las 12 combinaciones
```
Después: fader y MUTE con el ruido rosa (sin chasquidos), Smaart con la referencia sin tocar, y la persistencia al reiniciar.

## 0sexies. Revisión 11 — pantalla principal simplificada y manual en la app (sobre `cf5f158`)

Encargo: compactar los receptores, quitar el distintivo «REFERENCIA LOCAL» y mover la leyenda y las explicaciones a una ventana de Ayuda con el manual completo. Antes de modificar se hizo una copia de seguridad del proyecto en `cf5f158`. **No se instaló nada ni se reinició coreaudiod.** **Solo cambia la app:**
- el driver y `avmctl` de `dist/` son idénticos a los de `cf5f158`;
- `Client/avm_client.c` no cambia (`avmctl` sigue mostrando «Smaart Entrada 8 (REFERENCIA LOCAL)» en Terminal);
- la matriz, los presets y el bucle Smaart Salida 1 → Entrada 8 no cambian.

**Qué se comprobó y dónde:**
- **En Linux:**
  - el manual (`tests/test_manual.c`), con estructura, UTF-8 y los temas pedidos;
  - las comprobaciones estáticas de la app;
  - la compilación cruzada de la app y de las dos pruebas nativas;
  - las maquetas con la geometría y el texto reales.
- **En un Mac:** nada todavía. `tests/test_main_ui.m` está escrita y compilada, **sin ejecutar**.

### 0.11.1 Receptores compactos (`App/AVMatrixApp.m:153`)

- **Ancho:** la zona izquierda pasa de 386 a **268 pt**, y la matriz queda 118 pt más cerca.
- **Fila de cada receptor:**

  | Elemento | Posición |
  | --- | --- |
  | Número | 6-20 pt |
  | Nombre | 24-142 pt (118 pt) |
  | Barra | 150-206 pt |
  | dBFS | 210-250 pt, alineado a la derecha |
  | LED de saturación | 254-262 pt |

  El texto «SAT» de la esquina sube una línea para no pegarse a «dBFS».
- **Nombres largos:** se recortan con «…». La **ayuda emergente** (`rebuildToolTips`, `:305`) muestra «número · nombre», la descripción larga y «Doble clic para cambiar el nombre», en receptores y transmisores. Se calcula al mostrarse, así que sigue a los cambios de nombre. La zona de doble clic y el menú contextual usan las mismas medidas.
- **Se conservan:** números, nombres personalizados, barras, dBFS, saturación, Σ, grupos desplegables, bloqueos, la ventana que se ajusta a la matriz, el icono y el archivo de nombres.

### 0.11.2 Sin el rótulo «REFERENCIA LOCAL»

- Se quitan el distintivo naranja y el fondo anaranjado de la fila de la Entrada 8; `kRefBadgeW` desaparece. La Entrada 8 se dibuja como cualquier receptor y se puede renombrar igual.
- **En las descripciones de rutas** de la app, la Entrada 8 sigue indicando su función, ahora en minúsculas: «Smaart Entrada 8 «alias» (referencia local)». Así coincide con el nombre de canal que da el driver.

### 0.11.3 Ventana de Ayuda (`AVMHelpController`, `:917`)

- **Cómo se abre:**
  - botón **«Ayuda»** al final de la barra superior;
  - menú **Ayuda → Manual de AV Matrix** (⌘?), que es además el `NSApp.helpMenu` de macOS.
- **La ventana:**
  - independiente, redimensionable, con tamaño mínimo de 560 × 360 y posición recordada;
  - **índice** a la izquierda (`NSTableView`, títulos en hasta 2 líneas) y **texto con desplazamiento** a la derecha (`NSTextView` de solo lectura, seleccionable, que se ajusta al ancho);
  - un clic en el índice lleva el título de la sección arriba (`showSection:`, `:1073`), y al desplazar el texto el índice marca la sección visible (`textScrolled:`, `:1099`);
  - cerrar y volver a abrir reutiliza la misma ventana.
- **El manual** (`App/avm_manual.[ch]`, C puro y UTF-8) está compilado en la app: no necesita internet ni archivos. Tiene 13 secciones en español sencillo:
  1. qué es AV Matrix y la pantalla principal;
  2. los tres dispositivos y cómo seleccionarlos;
  3. transmisores, receptores y conexiones;
  4. grupos, colores, sumas y bloqueos;
  5. presets;
  6. referencia en Smaart;
  7. envío y retorno con LISTENTO;
  8. música estéreo;
  9. medidores (dBFS, saturación, silencio e inactividad);
  10. nombres;
  11. frecuencias y formatos;
  12. diagnóstico de problemas;
  13. limitaciones conocidas, incluidos **los silencios pendientes a 96 kHz** y que la compatibilidad con Smaart y LISTENTO y la precisión por internet **no están verificadas del todo**.
- El marcado mínimo (párrafos, «### » y «- ») se convierte en texto con estilo (`ManualText`, `:935`). El texto nunca se usa como cadena de formato.
- **Pantalla principal:** se quita la leyenda inferior (`:1315`). Se conservan la barra de controles, la línea de estado, la matriz y la descripción de rutas bajo la matriz.

### 0.11.4 Pruebas

| Prueba | Dónde | Qué comprueba |
| --- | --- | --- |
| `tests/test_manual.c` (nueva, 278) | Linux | 13 secciones completas con identificadores y títulos únicos; UTF-8 estricto; marcado bien formado; los **34 temas pedidos** (dispositivos, selección, matriz, grupos, colores, Σ, bloqueos, los tres presets, referencia en Smaart, envío y retorno, música L/R, medidores, dBFS, saturación, «−∞», «—», cambiar, guardar y restaurar nombres, frecuencias, Reparar frecuencia, formatos, diagnóstico, limitaciones, **silencios a 96 kHz**, sin internet, nombre completo al pasar el ratón); hechos que deben coincidir con el código (preset, bloqueo, 2 s, 0,25 s, 32 caracteres); sin «REFERENCIA LOCAL» ni «distintivo». Mutación (quitar la frase de 96 kHz): detectada |
| `tests/test_main_ui.m` (nueva, nativa) | Mac (sin ejecutar) | **Geometría:** kLeftW ≤ 280; número, nombre, barra, dBFS y LED sin solaparse; «+12.3» cabe; tamaño de la matriz desplegada. **Nombres:** la Entrada 8 sigue siendo editable; la ayuda emergente muestra el alias de 32 caracteres completo con número y dispositivo, también en un transmisor (Música L); no la hay sobre el medidor; recorte con «…» dentro de su columna; el preset de medición no cambia. **Ventana principal:** menú Ayuda → Manual (acción y `helpMenu`); botón «Ayuda»; se conservan los 7 botones, el selector de frecuencia, el estado y la descripción de rutas; sin leyenda. **Manual en pantalla:** abre, redimensionable, con desplazamiento, índice con las 13 secciones, todas en el texto y sin marcado visible; **cada entrada del índice lleva a su título**, a tamaño normal y en una ventana de 600 × 380; al desplazar se marca la sección; el texto sigue el ancho; cerrar y reabrir. Con `AVM_UI_NO_WINDOW=1` se omite la parte en pantalla |
| `tests/test_app_strings.sh` (+8, 28) | Linux | Sin distintivo ni `kRefBadgeW`; kLeftW ≤ 280; sin leyenda; botón y menú de Ayuda; ayuda emergente registrada; el manual pasa por `UTF8()` y nunca como formato; `avm_manual.c` en UTF-8. Mutaciones (kLeftW = 386; volver a poner la leyenda): detectadas |
| `tests/test_build_script.sh` (+3, 23) | Linux | `build.sh` incluye `avm_manual.c` en la app y ejecuta `test_manual` y `test_main_ui` |

**Resultados (Linux):**

| Suite | Resultado |
| --- | --- |
| Núcleo | 122/122 |
| Frecuencia | 115/115 |
| Medidores | 117/117 |
| Nombres | 51/51 |
| Manual | 278/278 |
| Instalador | 102/102 (`--app-only` sin cambios) |
| `build.sh` | 23/23 |
| Textos | 28/28 |

ASan y UBSan sin avisos (también en el manual). Compilación cruzada sin avisos.

**Maquetas** (Linux, no son capturas del Mac):
- `AVMatrix_compacta_claro.png` y `AVMatrix_compacta_oscuro.png`, con la ayuda emergente sobre la Entrada 8;
- `AVMatrix_ayuda.png`, la ventana del manual con el texto real.

**Pendiente en el Mac:**
```bash
AVM_UI_NO_WINDOW=1 bash scripts/build.sh   # test_rename_ui y test_main_ui sin ventanas
bash scripts/build.sh                      # con ventanas: «Pantalla principal y ayuda: … 0 fallos»
bash tests/mutate_rename_ui.sh
sudo bash scripts/install.sh --app-only --dry-run   # y después sin --dry-run
```

## 0quinquies. Revisión 10 — ancho fijo del panel de nombres y espera de la prueba de liberación (sobre `2ae5475`)

Codex ejecutó `AVM_UI_NO_WINDOW=1 bash scripts/build.sh` en el Mac: `test_rename_ui` dio 16 correctas y 3 fallos. `--app-only` pasó con firmas reales (incluida la restauración) y **no cambia**. Antes de modificar se hizo una copia de seguridad del proyecto en `2ae5475`. **No se instaló nada ni se reinició coreaudiod.** Solo cambian la app y sus pruebas: el driver y `avmctl` de `dist/` siguen idénticos.

**Qué se comprobó y dónde:**
- **En Linux:** compilación cruzada de la app, de la prueba nativa y de un mutante (arm64 y x86_64), comprobaciones estáticas y todas las suites.
- **En un Mac:** nada nuevo todavía. `test_rename_ui` y `tests/mutate_rename_ui.sh` **no se han ejecutado**.

### 0.10.1 Ancho del panel (`App/AVMatrixApp.m:726-790`)

**Causa probable:** NSStackView no deja recortar sus filas (su resistencia al recorte es obligatoria por defecto). El título de una sola línea, con un alias largo en la Entrada 8, pedía unos 567 pt. La pila se ensanchaba a 591 pt e invalidaba la restricción de 420 pt, y el popover se quedaba en 420.

**Corrección:**
- **Ancho de cada fila:** todas (título, subtítulo, campo, error, ayuda y botones) tienen un ancho **obligatorio** igual al interior (420 − 24 = 396 pt), con resistencia horizontal a la compresión baja (`:771`). El texto cede; el panel no.
- **Textos:**
  - el **título** pasa a ser una etiqueta de **hasta 2 líneas** (`:726`), con «…» al final si aún no cabe, así que se lee entero el alias de 32 caracteres con «(REFERENCIA LOCAL)»;
  - el **subtítulo** usa una línea recortada en el centro;
  - la **ayuda** y el **error** usan varias líneas, con `preferredMaxLayoutWidth` = 396.
- **Tamaño** (`updateSize`, `:781`): el ancho es siempre `kRenameW` y solo el alto sale de `fittingSize`. Mostrar un error **aumenta el alto sin ensanchar**.

### 0.10.2 Prueba de liberación (`tests/test_rename_ui.m`)

- **Espera acotada:** `wait_released` (`:37`) ejecuta el bucle principal (20 ms por vuelta) hasta que no quede ningún objeto vivo o se agote el plazo: 2 s sin mostrar el panel y 3 s mostrado. Así se cubre el *autorelease* diferido de AppKit que vio Codex (50 controladores vivos al vaciar el pool, 0 tras 0,5 s).
- **Control dentro de la propia prueba:** `test_detects_real_cycle` (`:155`) crea un ciclo real con `objc_setAssociatedObject` (el controlador retiene su popover). Comprueba que **tras la espera siguen vivos los dos objetos** (la espera no oculta fugas) y que se liberan al romper el ciclo.
- **Mutaciones nativas:** `tests/mutate_rename_ui.sh` (solo Mac) compila la prueba contra copias alteradas de la app (`-DAVM_APP_FILE`) y exige que falle con el motivo esperado:
  - **M1:** `popover` vuelve a ser `strong` (el ciclo original). Debe dar «siguen vivos».
  - **M2:** sin el ancho fijo de las filas (el error original). Debe dar «título largo».

  Las copias se crean y se borran junto a la app; el original no se toca. En Linux se comprobó que los dos patrones cambian el archivo y que el mutante M1 compila; **la ejecución queda pendiente en el Mac**.
- **Más comprobaciones:**
  - el ancho debe ser exactamente `kRenameW`;
  - `rowsInside`: ninguna fila visible sale de la vista, también con el error;
  - los mensajes de fallo muestran los tamaños.
- Se quitó una comprobación sin valor (campo editable con U+FFFE).
- **Total:** 22 comprobaciones con `AVM_UI_NO_WINDOW=1` y 40 con el panel en pantalla.

### 0.10.3 Resultados (Linux)

| Suite | Resultado |
| --- | --- |
| Núcleo | 122/122 |
| Frecuencia | 115/115 |
| Medidores | 117/117 |
| Nombres | 51/51 |
| Instalador | 102/102 (sin cambios, incluido `--app-only`) |
| `build.sh` | 20/20 |
| Textos | 20/20 (+3: ancho fijo por fila, ancho de `updateSize`, sin `MAX` con `fittingSize`) |

Sanitizers sin avisos. Compilación cruzada sin avisos.

**Pendiente en el Mac:**
```bash
AVM_UI_NO_WINDOW=1 bash scripts/build.sh     # «Panel de nombres: 22 comprobaciones correctas, 0 fallos»
bash scripts/build.sh                        # con el panel en pantalla: 40 correctas
bash tests/mutate_rename_ui.sh               # «Mutaciones del panel: 2 detectadas, 0 fallos»
```

## 0quater. Revisión 9 — tamaño del panel de nombres y ciclo de retención (sobre `277acc5`)

Codex comprobó `277acc5` en el Mac (Enter, Escape, restauración, persistencia, tildes y fallo de guardado correctos) y encontró dos problemas en el panel de nombres. Antes de modificar se hizo una copia de seguridad del proyecto en `277acc5`. **No se instaló nada ni se reinició coreaudiod.** **Solo cambia la app:** el driver y `avmctl` de `dist/` son byte a byte iguales a los de `277acc5`.

**Qué se comprobó y dónde:**
- **En Linux:** compilación cruzada de la app y de la prueba nativa (arm64 y x86_64, `-Werror`), comprobaciones estáticas, pruebas del instalador y mutaciones.
- **En un Mac:** nada todavía. La prueba nativa `tests/test_rename_ui.m` **está escrita y compilada, pero no se ha ejecutado**: necesita AppKit. La ejecuta `scripts/build.sh`.

### 0.9.1 Tamaño del panel (`App/AVMatrixApp.m`)

- **Ancho fijo** `kRenameW` = 420 pt y **tamaño explícito** antes de mostrar el panel:
  - `updateSize` (`:764`) calcula `fittingSize`, fija el `frame` de la vista, `preferredContentSize` y `popover.contentSize`;
  - `AVMMakeRenamePopover` (`:855`) carga la vista y asigna `pop.contentSize = rc.preferredContentSize` después de `contentViewController`. Así no queda en 0 × 0.
- **Mensaje de error sin recortes:**
  - `wrappingLabelWithString` con `preferredMaxLayoutWidth` y ancho igual al contenido (`:733`): un mensaje largo pasa a varias líneas;
  - `showMessage:` (`:778`) lo muestra y **recalcula el tamaño**: el panel crece en alto, nunca en ancho. Lo usan los tres errores: demasiado largo, UTF-8 inválido y fallo al guardar.
- Sin cambios en el contenido, los textos, Enter, Escape, restaurar ni el guardado y su reversión.

### 0.9.2 Ciclo de retención

- **Antes:** `AVMController.renamer` → controlador → `popover` (fuerte) → `contentViewController` → controlador.
- **Ahora** (`:700`, `:884`, `:1187`):
  - el `AVMController` retiene **el popover** (`renamePopover`) solo mientras está abierto;
  - el popover retiene al controlador (`contentViewController`);
  - el controlador guarda el popover con referencia **débil**;
  - `popoverDidClose:` (el `AVMController` es el delegado) suelta el popover; se libera en la siguiente vuelta del bucle (`dispatch_async`), nunca durante una acción del propio panel (Guardar, Restaurar, Cancelar);
  - abrir otro canal con el panel abierto cierra y suelta el anterior.
- El bloque `onChange` captura el `AVMController` con referencia débil: el panel tampoco lo retiene.

### 0.9.3 Pruebas

| Prueba | Dónde | Qué comprueba |
| --- | --- | --- |
| `tests/test_rename_ui.m` (nueva, nativa) | Mac (sin ejecutar) | Incluye la app sin su `main` (`AVM_APP_NO_MAIN`). **Tamaño:** canales Salida 1, Música R, Entrada 8 y Envío 8: `contentSize` ≥ 300 × 100, igual al `frame` de la vista y ≥ `fittingSize`. **Error:** alias de más de 32 caracteres rechazado (no cambia el nombre), el panel crece, el mensaje cabe entero (alto necesario ≤ alto del marco y dentro de la vista); también con el mensaje de fallo al guardar y uno de 300 caracteres; el ancho no cambia. **Retención sin mostrar:** 50 paneles creados y soltados, 0 vivos. **Mostrado en pantalla** a través del `AVMController`: 20 aperturas alternando Cancelar/Escape, cierre exterior y abrir otro canal con uno abierto; todos visibles, con tamaño correcto en su ventana y liberados al final; el `AVMController` tampoco queda retenido. `AVM_UI_NO_WINDOW=1` omite la parte en pantalla. No escribe en el archivo de nombres |
| `tests/test_app_strings.sh` (+5, 17) | Linux | `popover` débil, ningún controlador retenido aparte, `contentSize` explícito, `showMessage:` recalcula, `popoverDidClose:` suelta el panel. Mutaciones (`strong`; sin `contentSize`): detectadas |
| `tests/test_build_script.sh` (+1, 20) | Linux | `build.sh` compila la prueba nativa con la app y la ejecuta |
| `tests/test_install.sh` (+19, 102) | Linux | Nuevo `install.sh --app-only` (ver abajo). Mutaciones (instalar los tres componentes; no salir antes del reinicio): 6 y 1 fallos |

### 0.9.4 Actualizar solo la app: `install.sh --app-only`

- Valida, prepara, respalda y sustituye **solo** `/Applications/AV Matrix.app`, con la misma transacción y el mismo rollback.
- **Nunca reinicia coreaudiod.** No necesita el driver ni `avmctl` en el paquete.
- Conserva el icono instalado (misma lógica de firma: nunca degrada un Developer ID).
- Los nombres de los canales no se tocan: están en Application Support.
- Probado: el driver y `avmctl` quedan byte a byte iguales, el respaldo contiene solo la app, una firma inválida no cambia nada y un fallo en la sustitución restaura la app anterior.

**Resultados (Linux):**

| Suite | Resultado |
| --- | --- |
| Núcleo | 122/122 |
| Frecuencia | 115/115 |
| Medidores | 117/117 |
| Nombres | 51/51 |
| Instalador | 102/102 |
| `build.sh` | 20/20 |
| Textos | 17/17 |

ASan, UBSan y TSan sin avisos. Compilación cruzada sin avisos.

**Pendiente en el Mac:**
- `bash scripts/build.sh`, que ejecuta `test_rename_ui`: debe acabar con «Panel de nombres: … 0 fallos».
- Ver el panel en pantalla, con un error visible.
- `sudo bash scripts/install.sh --app-only`.

## 0ter. Revisión 8 — nombres personalizados de los canales (sobre `453bf79`)

Encargo: poder renombrar los 34 canales (Smaart Salidas 1–8, LISTENTO Retornos 1–8, Música L/R, Smaart Entradas 1–8 y LISTENTO Envíos 1–8). Antes de modificar se hizo una copia de seguridad del proyecto en `453bf79`. **No se instaló nada ni se reinició coreaudiod.** **El driver no cambia:** los nombres son solo de la app.

**Qué se comprobó y dónde:**
- **En Linux:** el módulo de nombres (lógica, normalización, tildes, persistencia), la compilación cruzada de la app con el SDK 15.5 y una maqueta de la interfaz.
- **En un Mac:** nada todavía. La edición en la interfaz (doble clic, Enter, Escape, panel, menú del clic derecho) **solo se compiló y se maquetó; no se ha ejecutado**.

### 0.8.1 Diseño

- **Módulo C portable** `Client/avm_names.[ch]` (sin CoreAudio ni AppKit), probado en Linux. La app solo lo llama.
- **Identificación estable:** cada alias se guarda con una clave por dispositivo, dirección y número: `smaart.out.1-8`, `listento.out.1-8`, `music.out.1-2`, `smaart.in.1-8` y `listento.in.1-8`. No depende del orden interno.
- **Normalización** (`avm_names_normalize`): UTF-8 estricto (se rechazan secuencias sobrelargas, sustitutos y Latin-1 suelto); tabuladores y saltos de línea pasan a espacio; se recortan los extremos y se unen los espacios repetidos; se quitan los caracteres de control; máximo **32 caracteres** (no bytes). La app aplica antes NFC (`precomposedStringWithCanonicalMapping`), así «é» escrito con acento combinante se guarda igual que «é».
- **Restaurar:** un alias vacío o igual al original equivale a «Restaurar nombre original». Un alias rechazado (demasiado largo o inválido) **deja el anterior intacto**.
- **Persistencia:** `~/Library/Application Support/AV Matrix/nombres_canales.txt`, cabecera `AVMATRIX-NOMBRES 1` y líneas `clave<TAB>alias`. Guardado atómico (`.tmp.<pid>`, `fsync`, `rename`). Fuera del bundle: sobrevive a cerrar la app, a reiniciar el Mac y a actualizar o reinstalar. `uninstall.sh` no lo borra.
- **Carga tolerante:** se ignoran claves desconocidas (versiones futuras) y líneas inválidas; se descartan líneas demasiado largas; se aceptan CRLF y una última línea sin salto. Si el archivo no tiene cabecera, la app lo aparta como `nombres_canales.txt.dañado-<fecha>` y empieza con los nombres originales (`App/AVMatrixApp.m:1116`).
- **Si falla el guardado,** la app revierte el cambio en memoria y lo avisa en el panel; nunca muestra un nombre que no esté en disco.

### 0.8.2 Interfaz (`App/AVMatrixApp.m`)

- **Doble clic** sobre el nombre de un canal (`:650`) o **clic derecho** → «Cambiar nombre…» / «Restaurar nombre original» (`:622`).
- Panel `AVMRenameController` (`NSPopover` transitorio, `:690`): título, «Original: … · identificador: clave», campo, mensaje de error, botones Restaurar, Cancelar y Guardar. **Enter** guarda y **Escape** cancela (`:760`); un clic fuera también cancela.
- **El número siempre queda visible:** arriba «3 · Nombre» (`TxChannelName`, `:72`); a la izquierda, en su propia columna. El texto largo se recorta con «…» sin tapar el número ni el distintivo.
- **Entrada 8:** con cualquier alias conserva el distintivo **REFERENCIA LOCAL** en su fila y en la descripción de la ruta: `Smaart Entrada 8 «alias» (REFERENCIA LOCAL)` (`DestLongName`, `:95`).
- **Descripciones de las rutas** (barra de estado, ayudas): `Smaart Salida 1 «Generador rosa» → …`.
- Se amplió la zona de nombres de los receptores (columna izquierda de 336 a 386 pt) para que quepa «Señal de referencia» junto al distintivo. Medidores, icono, presets y grupos desplegables no cambian.
- **Renombrar no envía nada al driver:** ni la matriz `'mtrx'`, ni la frecuencia, ni los niveles. Los nombres que ven Smaart y LISTENTO (del driver) no cambian.
- Textos con `UTF8()` y `%@`; un alias nunca se usa como cadena de formato (comprobado en `tests/test_app_strings.sh`).

### 0.8.3 Pruebas (`tests/test_names.c`, 51 comprobaciones; Linux)

| Grupo | Qué comprueba |
| --- | --- |
| Claves y nombres | 34 claves únicas y estables, ida y vuelta clave ↔ índice; nombres originales («Salida 3», «Retorno 1», «Música L», «Entrada 8», «Envío 2»); números 1–8 y L/R |
| Edición | Asignar, cambiar y leer alias; el resto de canales no cambia |
| Cancelación | Un alias rechazado deja el anterior intacto (equivale a cancelar en la lógica; el botón y Escape de la interfaz no se pueden ejecutar en Linux) |
| Restaurar | `avm_names_restore`, alias vacío, solo espacios e igual al original |
| Tildes | «Señal pública · Ñandú ÁÉÍÓÚ üç», eñes, acento combinante; 32 «é» se admiten y 33 se rechazan |
| Normalización | Espacios, tabuladores, saltos de línea, controles; UTF-8 inválido, sobrelargo y sustituto rechazados |
| Persistencia | Guardar y recargar idéntico (con tildes), sin archivo temporal sobrante, la restauración persiste, contenido exacto del archivo, sin archivo = originales |
| Carga robusta | Claves desconocidas, líneas sin tabulador, CRLF, última línea sin salto, línea gigante, alias inválido; archivo sin cabecera = false |
| Errores | Guardar en una ruta imposible devuelve false |
| Mutaciones | Clave cambiada (2 fallos), sin recortar espacios (3), sin validar UTF-8 (4): las tres se detectan |

**Resultados (Linux):** nombres 51/51, núcleo 122/122, frecuencia 115/115, medidores 117/117, instalador 83/83, `build.sh` 19/19 (antes 17; comprueba que la app incluye `avm_names.c`), textos 12/12 (antes 8). ASan y UBSan sin avisos, también en `test_names`. Compilación cruzada de driver, app y `avmctl` (arm64 y x86_64) sin avisos con `-Werror`.

**Pendiente en el Mac** (lista en `INSTALAR.md`):
- Doble clic, Enter, Escape, clic fuera y menú del clic derecho.
- Que el alias aparezca en las cabeceras y en la barra de estado, con tildes.
- Que la Entrada 8 conserve REFERENCIA LOCAL.
- Que los nombres sigan tras cerrar y abrir la app, reiniciar el Mac y reinstalar.
- Que renombrar no cambie conexiones, niveles ni el audio en Smaart y LISTENTO.

## 0bis. Revisión 7 — saturación exacta según el redondeo real (sobre `2e77f49`)

Codex comprobó `2e77f49` en el Mac: compilación, pruebas, sanitizers y conservación del icono en una instalación simulada. Encontró un caso límite en `avm_format_delivery_clip_level`: `nearbyint` redondea al par, así que 32766,5 → 32766 (SInt16) y 8388606,5 → 8388606 (SInt24) no entregan el código máximo, pero el destino marcaba SAT por usar `>=`. Antes de modificar se hizo una copia de seguridad del proyecto en `2e77f49`. **No se instaló nada ni se reinició coreaudiod.** Todo lo de esta sección se probó **solo en Linux**.

### 0.7.1 Corrección (`Driver/src/avm_core.c:311`)

- Se elimina el umbral fijo. La nueva función `avm_format_is_full_scale(máx, mín, formato)` convierte los extremos con signo de cada bloque con **la misma conversión de entrega** (`avm_quantize` → `avm_float_to_sample`: `nearbyint` al par y recorte). Hay saturación si el resultado es **el código máximo o el mínimo** del formato (en Float32: ≥ 1,0 o ≤ -1,0).
- Así la indicación coincide por construcción con lo entregado, incluida la asimetría del redondeo al par:
  - SInt16: +32766,5 → 32766 (sin SAT); −32767,5 → −32768 (SAT);
  - SInt24: +8388606,5 → 8388606 (sin SAT); −8388607,5 → −8388608 (SAT).
- **Fuentes y destinos usan la misma función** (`avm_core.c:507` y `:607`). Los medidores registran ahora el máximo y el mínimo con signo de cada bloque; el pico mostrado sigue siendo el máximo absoluto.
- **Cambio de comportamiento a revisar:** en una fuente entera, el código −32767 (o −8388607) ya **no** marca SAT, porque no es el mínimo del formato. Antes se marcaba al comparar el valor absoluto. Ahora fuente y destino coinciden en eso.
- **Se conserva:** saturación por suma (cualquier suma ≥ 1,0 se convierte al código máximo en los cuatro formatos), SInt32 y Float32 a ±1,0, y el seqlock de sesión.
- **`avmctl test`** (`Client/avmctl.c:456`) calcula el SAT esperado del Envío 1 con la misma función.

### 0.7.2 Pruebas nuevas (`tests/test_meters.c`)

| Prueba | Qué comprueba |
| --- | --- |
| `test_exact_rounding_points` (`:682`) | SInt16 y SInt24 **en el punto exacto y a ambos lados** (`nextafterf`), positivos y negativos: +32766,5 (32766, sin SAT), por encima (32767, SAT), por debajo (32766, sin SAT); +32767; +32767,5 (recortado a 32767, SAT); −32767,5 (−32768, SAT), hacia cero (−32767, sin SAT), más negativo (−32768, SAT); −32767 y −32766,5 (sin SAT); −1,0 (SAT). Lo mismo con 8388606,5 y −8388607,5 en SInt24. SInt32 y Float32 a ±1,0 y justo dentro. En cada caso se comprueba el **código entregado** y el SAT |
| `test_clip_matches_delivery_sweep` (`:734`) | Barrido de **todos los float** alrededor de ±escala completa en los 4 formatos: SAT ⇔ el código entregado es el máximo o el mínimo |
| `test_source_codes_near_full_scale` (`:765`) | Fuentes SInt16 y SInt24 con los códigos máx, máx−1, máx−2, mín, mín+1 y mín+2: SAT en fuente y destino solo con máx y mín |
| Mutaciones | Umbral anterior (`>=` 32766,5/24 bits): 11 fallos. Negativo simétrico (|mín| ≥ máx): 8 fallos | Las dos se detectan |

**Resultados:** medidores 117/117 (antes 83; las anteriores siguen pasando), núcleo 122/122, frecuencia 115/115, instalador 83/83, `build.sh` 17/17, textos 8/8. ASan, UBSan y TSan sin avisos. Compilación cruzada de driver, app y `avmctl` sin avisos.

**Pendiente en el Mac:** `avmctl test` y `avmctl test-all` con los medidores reales.

## 0septies. Revisión 6 — saturación por formato, autoprueba SInt16, limpieza entre sesiones e icono (sobre `b6b52f0`)

Codex compiló los tres componentes en el Mac y reprodujo cuatro problemas. Antes de modificar se hizo una copia de seguridad del proyecto en `b6b52f0`. **No se instaló nada ni se reinició coreaudiod.**

**Qué se comprobó y dónde:**
- **En Linux:** todo lo de esta sección.
- **En un Mac:** nada nuevo. Lo único verificado en Mac sigue siendo lo que Codex hizo sobre `b6b52f0`: compilación y las pruebas incluidas.

### 0.6.1 Saturación en los destinos según el formato (`Driver/src/avm_core.c`)

- **Causa:** el destino comparaba `|suma| ≥ 1,0`. Con SInt16, la muestra 32767 equivale a 0,999969482 < 1,0: la muestra entregada era de escala completa, pero SAT seguía apagado.
- **Corrección:** `avm_format_delivery_clip_level(fmt)` (`avm_core.c:311`) devuelve el umbral a partir del cual la conversión al formato entregado produce el código de escala completa (con el redondeo al más cercano que usa el driver):
  - Float32 y SInt32: 1,0;
  - SInt24: 8388606,5/2^23;
  - SInt16: 32766,5/32768.
- El destino usa ese umbral con su formato (`avm_core.c:605`). Así, **SAT se enciende exactamente cuando la muestra entregada es de escala completa**, también en negativo.
- **Exceso por suma conservado:** toda suma ≥ 1,0 supera cualquier umbral, en los cuatro formatos.
- **SInt32:** el transporte en float no distingue 2^31-1 de 2^31-2; el primer código por debajo de escala completa en float es 2^31 - 2^8. Está documentado en la prueba.

### 0.6.2 Autoprueba con SInt16 (`Client/avmctl.c:439`)

- **Causa:** `meters_during_clip` esperaba 0,6 y 1,2 exactos, pero en SInt16 llegan 19661/32768 = 0,600006104 y 1,200012207.
- **Corrección:**
  - Los valores esperados se calculan con los **formatos físicos reales**: salidas de Smaart para las fuentes y entradas de LISTENTO para la saturación del destino (`physical_format_of`, `:429`).
  - Se usa `avm_quantize` (`avm_core.c:322`), la misma conversión de ida y vuelta del driver.
  - El SAT esperado del Envío 1 se calcula con el umbral del formato del destino.
- **La tolerancia sigue en 1e-6.** Las comprobaciones de aislamiento y de silencio no cambian: siguen siendo exactas.
- **Prueba en Linux** (`test_selftest_clip_expectations`, `tests/test_meters.c:589`): reproduce esa fase en las **16 combinaciones** de formato de salida × formato de entrada, con conversión float → formato como el HAL. Comprueba los 6 medidores con tolerancia 1e-6 y los SAT esperados. También confirma que en SInt16 los valores reales difieren de 0,6 y 1,2 más que la tolerancia, que era el fallo de la prueba anterior.

### 0.6.3 Limpieza de medidores entre sesiones (`avm_core.c:406`, `:253`)

- **Causa:** `avm_engine_reset` vaciaba el buffer, pero no los medidores. Tras un reinicio, en los 250 ms siguientes se veían los picos de la sesión anterior.
- **Corrección:** `avm_engine_reset`, que el driver llama en `StartIO` cuando empieza una sesión nueva, limpia los 34 medidores dentro de un **seqlock de sesión** (`meter_session`):
  - lo pone impar, limpia, y lo pone par con `release`;
  - `avm_engine_meter_snapshot` hace una sola pasada sin reintentos: si ve el contador impar, o cambiado durante la lectura, informa todo inactivo;
  - nunca mezcla valores de dos sesiones;
  - todos los accesos siguen siendo atómicos.
- **Supuesto:** como antes, no hay IO durante `reset`. El driver lo llama solo cuando `running_total` pasa de 0 a 1, protegido por `gClockMutex`.

### 0.6.4 Icono de la app

- **El archivo `AVMatrix.icns` de Antonio no está en el repositorio ni en esta sesión. No se ha fabricado ningún icono.** Para que no vuelva el icono genérico, el flujo es este:
  - **`App/Info.plist`:** `CFBundleIconFile = AVMatrix`.
  - **`scripts/resolve_icon.sh`** (nuevo, usado por `build.sh` y por `make dist`): toma el icono de `AVM_ICON_SOURCE`, de `App/Resources/AVMatrix.icns` o de la app instalada. En el último caso lo importa a `App/Resources/` para las compilaciones siguientes. De la app instalada lo localiza por su `CFBundleIconFile`, por `AVMatrix.icns` o por cualquier `.icns` de sus recursos. Comprueba la firma «icns» y que el `Info.plist` lo declare.
  - **`build.sh`:** copia el icono a `Contents/Resources/AVMatrix.icns` **antes de firmar**, para que quede sellado. **Si no hay icono, se detiene** con instrucciones (`AVM_ALLOW_GENERIC_ICON=1` permite continuar).
  - **`make dist`:** lo incluye si existe `App/Resources/AVMatrix.icns`. En esta sesión no existe, así que **el `dist/` de este commit no trae icono**.
  - **`install.sh`:**
    - si el paquete trae icono, lo valida (firma «icns» y declaración en el plist);
    - si no lo trae y la app instalada sí, **lo conserva**: lo copia a la app preparada y rehace la firma;
    - la firma se rehace ad hoc si era ad hoc; una firma con identidad solo se rehace con `--sign-identity=<identidad>` y **nunca se degrada a ad hoc** (el instalador se detiene).
- **Para dejarlo fijo en el repositorio:** copiar el icono de Antonio a `App/Resources/AVMatrix.icns` y hacer commit, o dejar que `build.sh` lo importe en el Mac y hacer commit de ese archivo.

### 0.6.5 Pruebas

| Prueba | Resultado (Linux) |
| --- | --- |
| `test_meters.c`: SInt16, SInt24 y SInt32 a escala completa (fuente y destino con SAT, código entregado máximo, mismo pico), código inferior (sin SAT), negativo a escala completa; float 0,99996 entregado en SInt16 como 32767 → SAT; en Float32 → sin SAT; suma 0,6 + 0,6 → SAT en los 4 formatos; cuantización (0,600006104 y 1,200012207); 16 combinaciones de la fase de saturación de `avmctl`; reinicio con consulta 10 ms después (los 34 inactivos, sin pico ni SAT); nueva sesión sin SAT heredada; reinicios con 2 lectores concurrentes (≈3,4 M lecturas) | **83/83**; ASan, UBSan y TSan sin avisos |
| Mutaciones con los fallos de Codex: destino comparado con 1,0 (3 fallos: SInt16, SInt24 y float → SInt16); reinicio sin limpiar medidores (4 fallos) | Se detectan ambas |
| `test_install.sh`: icono incluido; icono conservado con firma ad hoc rehecha en staging; Developer ID sin icono y sin identidad → error sin cambios; con identidad → conservado y firmado con ella; icono instalado con otro nombre; `.icns` inválido o no declarado → error por ese motivo, sin cambios | **83/83** |
| `test_build_script.sh`: icono incluido y declarado; sin icono → error claro; importación desde la app instalada al proyecto (en rutas aisladas) y dentro de la nueva app; `.icns` inválido → error | **17/17** |
| Núcleo, frecuencia y textos | 122/122, 115/115, 8/8 |
| Compilación cruzada de driver, app y `avmctl` (SDK 15.5, arm64 + x86_64, `-Werror`) | Sin avisos |

**Pendiente en el Mac:**
- `avmctl test` y `avmctl test-all` con los medidores reales, en especial con SInt16;
- ver el icono tras `build.sh` + instalación (opción A) o tras instalar `dist/` (opción B, icono conservado).

## 0sexies. Revisión 5 — medidores de nivel en tiempo real (sobre `6658962`)

Petición de Antonio: medidores por canal (barra, dBFS y saturación) en las 18 fuentes y los 16 destinos, junto a los nombres. Se conservan la matriz tipo Dante, los grupos desplegables, la REFERENCIA LOCAL, las conexiones y los presets. Antes de modificar se hizo una copia de seguridad del proyecto en `6658962`. **No se instaló nada ni se reinició coreaudiod.**

### Qué se comprobó en un Mac y qué solo en Linux

- **En un Mac: nada de esta revisión.** Lo único verificado en Mac hasta ahora es lo que Codex comprobó sobre `de78154`: compilación, pruebas y matriz nativa. Los medidores no se han visto en pantalla ni se han medido con el driver cargado en coreaudiod.
- **En Linux:** todas las pruebas de esta sección (núcleo de medición, pruebas de mutación, sanitizers), compilación cruzada contra el SDK 15.5 y maquetas renderizadas con Chromium.
- **Preparado para el Mac:** `avmctl test` incluye ahora la comprobación de los medidores reales del driver (ver §0.5.4), e `INSTALAR.md` tiene la lista de comprobaciones manuales con Smaart.

### 0.5.1 Medición en el driver (`Driver/src/avm_core.[ch]`)

- **Fuentes** (`avm_engine_write_metered`, `avm_core.c:413`): pico absoluto de cada muestra **ya convertida a float y escrita en el buffer**, antes de la matriz.
- **Destinos** (`avm_engine_read_metered`, `avm_core.c:471`): pico absoluto del **valor sumado por la matriz** (`acc`), el mismo que se convierte y se entrega. Mide la señal real tras el enrutamiento y la suma, no la fuente. Si el bloque se descarta por vuelta del escritor, se mide el silencio entregado.
- **Saturación:**
  - fuente: |x| ≥ escala completa de su formato físico (`avm_format_clip_level`, `avm_core.c:289`: 1,0 para Float32 y SInt32, 8388607/8388608 para SInt24, 32767/32768 para SInt16);
  - destino: |suma| ≥ 1,0.
  - El indicador se mantiene 2 s.
- **Tiempo real** (`avm_meter_update`, `avm_core.c:211`):
  - un solo escritor por medidor (el hilo de IO de su dispositivo), solo cargas y almacenamientos atómicos;
  - sin bucles CAS, sin mutex, sin memoria nueva y sin llamadas al sistema;
  - la hora es `mach_absolute_time` convertida a ns, que ya se usaba.
  - El coste por bloque es un `fabsf` + comparación por muestra y canal, y unas pocas escrituras atómicas por canal.
- **Sin valores antiguos** (`avm_meter_read`, `avm_core.c:227`):
  - pico por ventanas de 50 ms; la lectura devuelve el máximo de la ventana actual y la anterior, **sin borrar**, así varios lectores (app, `avmctl`) no se roban picos;
  - sin bloques nuevos en 250 ms el medidor queda **inactivo** con pico 0.
- **Las variantes sin medición** (`avm_engine_write/read`) se mantienen como envoltorios con `now_ns = 0`, que no miden. Las pruebas existentes no cambian.

### 0.5.2 Propiedad `'metr'` (`Driver/src/AVMatrixDriver.c`)

- CFData de solo lectura (`AVM_GetMetersProperty`, `AVMatrixDriver.c:665`) en el plug-in y en los tres dispositivos. Formato: cabecera `'AVM1'` y 34 × (float pico, activo, saturación) (`avm_meters_pack/unpack`).
- El IO usa las variantes medidas (`AVMatrixDriver.c:1392`, `:1396`).
- **Leer la propiedad no abre ni arranca audio.**

### 0.5.3 App (`App/AVMatrixApp.m`)

- **Receptores:** a la derecha del nombre, barra horizontal de 64 pt, dBFS con un decimal y LED de saturación. La etiqueta REFERENCIA LOCAL de la Entrada 8 se mantiene.
- **Transmisores:** bajo el nombre vertical, LED de saturación, barra vertical de 50 pt y dBFS entero.
- **Textos:** «—» = sin audio, «−∞» = silencio digital. Colores de sistema (verde, amarillo, naranja, rojo y `labelColor` con transparencia) para los modos claro y oscuro.
- **Ratón:** la línea inferior da el nivel exacto, «antes de la matriz» o «entregado, después de la matriz», y la saturación.
- **Consulta** (`startMeters`, `:616`; `pollMeters`, `:626`): un temporizador lee `'metr'` 15 veces por segundo, solo con la ventana visible. Sin driver, todo se muestra como «—».
- **Geometría:** nombres 336 pt y cabecera 184 pt. La ventana sigue ajustándose a la matriz.

### 0.5.4 Pruebas

| Prueba | Dónde | Resultado |
| --- | --- | --- |
| `tests/test_meters.c` (nueva, 41 comprobaciones): silencio, señal conocida, separación de 18 canales, destinos tras la matriz, suma, saturación, sin valores antiguos, medición sin efecto sobre el audio, empaquetado y concurrencia (≈9 M lecturas, TSan) | Linux | **41/41**; ASan, UBSan y TSan sin avisos |
| Mutaciones: el destino mide el máximo de las fuentes en vez de la suma (1 fallo); valores antiguos visibles (4 fallos); saturación de destino desactivada (2 fallos) | Linux | Las tres se detectan |
| Núcleo, frecuencia, instalador, `build.sh` y textos | Linux | 122/122, 115/115, 57/57, 7/7, 8/8 |
| Compilación cruzada de driver, app y `avmctl` (arm64 + x86_64, `-Werror`); el driver solo exporta `_AVMatrix_Create` | Linux, SDK 15.5 | Sin avisos |
| Maquetas en modo claro y oscuro con la geometría de la app | Linux (Chromium) | Entregadas aparte; **no son capturas del Mac** |
| **`avmctl test` con los medidores reales:** nivel exacto (±1e-6) de las 34 líneas con valores distintos por canal (separación) y `smaart.in.1` = retorno 1 + retorno 2 (suma); saturación (Salida 1 a 0 dBFS y Envío 2 = 0,6 + 0,6 → SAT, Envío 3 sin ruta → silencio); los 34 inactivos 0,4 s después de parar; en `test-all`, las 12 combinaciones de frecuencia y formato | **Pendiente en el Mac** | Compila; no ejecutado |
| Lista manual de `INSTALAR.md` §6: nivel junto al de Smaart, «—» con LISTENTO cerrado, LED a 0 dBFS, «—» al detener, cerrar y abrir la app sin cortes | **Pendiente en el Mac** | — |

### 0.5.5 Límites y riesgos

- **Medidor de pico, no RMS ni LUFS.** No es un analizador; Smaart sigue haciendo el análisis.
- **El pico de una fuente** es lo que llega al driver tras la mezcla del HAL de macOS. Si dos apps envían a la misma salida, se ve la suma.
- **Un destino que nadie lee** (por ejemplo, LISTENTO cerrado) muestra «—»: no hay señal entregada que medir.
- **Inexacto por diseño en la pantalla:** la lectura es de picos en ventanas de 50-100 ms con consulta cada 67 ms, así que un pico de una sola muestra puede no verse si cae justo entre lecturas. La prueba lo comprueba con señales constantes.
- **Coste de la consulta:** 15 lecturas por segundo de una propiedad de 288 bytes a través de coreaudiod. Se espera un coste despreciable, pero no se ha medido.

## 0quinquies. Revisión 4 — textos UTF-8 en la app (sobre `de78154`)

Codex comprobó `de78154` en el Mac (compilación, pruebas y matriz nativa) y detectó que «Música» se mostraba como «M√∫sica».

- **Causa:** `stringWithFormat:` con `%s` interpreta los textos C con la codificación del sistema, no como UTF-8.
- **Corrección (`App/AVMatrixApp.m`):** nuevo `UTF8(const char*)`, que usa `stringWithUTF8String:` y devuelve `@""` si recibe NULL. Los cinco usos de `%s` pasan a `%@ + UTF8(...)`:
  - cabeceras de grupo de transmisores y receptores;
  - mensajes «Receptor …» y «Transmisor …: clic para plegar/desplegar»;
  - estado «falta <dispositivo>».
  - `SourceLongName` y `DestLongName` también usan `UTF8()`.
- **Alcance:** solo cambian los textos. Las rutas, la matriz y las demás funciones no cambian. El driver ya usaba `kCFStringEncodingUTF8` y `avmctl` escribe UTF-8 en la Terminal.
- **Prueba nueva `tests/test_app_strings.sh`** (en `make test` y en `build.sh`):
  - no queda ningún `%s` en cadenas de formato de la app;
  - todo `kTxGroups/kRxGroups[...].name` y `avm_device_label(...)` pasa por `UTF8()`;
  - `UTF8()` usa `stringWithUTF8String:`;
  - las fuentes son UTF-8 válido (`iconv`).
  - Pasa con 8/8. **Con el `AVMatrixApp.m` de `de78154` falla** en 4 comprobaciones y señala las 5 líneas.
- **Resto:** núcleo 122/122, frecuencia 115/115, instalador 57/57, `build.sh` 7/7; sanitizers sin avisos; compilación cruzada sin avisos.
- **Pendiente en el Mac:** ver en pantalla «⊟ Música», «Transmisor Música: clic para plegar» y el estado «falta AV Matrix Música» si faltara ese dispositivo.

## 0quater. Revisión 3 — matriz tipo Dante y referencia local (sobre `0e7c9e8`)

Petición de Antonio: organizar la matriz como Dante Controller / Dante Preset Creator, con la Entrada 8 como **REFERENCIA LOCAL**. Se conservan el controlador y todas las funciones. Antes de modificar se hizo una copia de seguridad del proyecto en `0e7c9e8`. **No se instaló nada ni se reinició coreaudiod.**

**Nota:** las capturas de Dante que se mencionan no llegaron a esta sesión. La organización sigue la convención estándar de Dante: transmisores arriba con nombres verticales, receptores a la izquierda, una columna o fila de cabecera por dispositivo con ⊟/⊞, y casillas verdes con ✓.

### 0.α Nueva pantalla principal (`App/AVMatrixApp.m`)

- La rejilla de `NSButton` se sustituye por una vista propia, `AVMMatrixView`, que dibuja la matriz completa con casillas de 20 pt.
  - **Transmisores arriba**, con nombres verticales: Smaart Salida 1-8, LISTENTO Retorno 1-8, Música L/R.
  - **Receptores a la izquierda:** Smaart Entrada 1-8, LISTENTO Envío 1-8.
  - Cada grupo tiene una cabecera plegable (⊟/⊞) y el estado se recuerda en `NSUserDefaults`.
- **Casillas:** verde con ✓ = conectada; vacía = desconectada; gris con × = bloqueada (lazo).
- **Cabeceras y grupos plegados:** un punto verde indica que hay conexiones dentro. Al pulsarlo se despliega; nunca modifica conexiones.
- **Entrada 8:** etiqueta naranja «REFERENCIA LOCAL» y fila resaltada.
- **Ratón:** resalta la fila y la columna y muestra abajo la ruta completa y su estado.
- **Σ n** en naranja si una fila suma varias fuentes.
- **Sin espacio vacío:** la ventana se ajusta al tamaño de la matriz (`fitWindow`) y se reajusta al plegar o desplegar. Por encima del tamaño de la pantalla, aparecen barras de desplazamiento.
- **Barra superior compacta:** frecuencia, «Preset medición», «Directo 8×8», «Vaciar», «Desplegar todo», «Plegar todo», «Recargar» y «Reparar frecuencia» (solo si hay inconsistencia).
- **Funciones conservadas:** escucha de `'mtrx'`, `'rsta'` y frecuencia; reparación de frecuencia; mensajes de rechazo.
- El clic en una casilla bloqueada solo emite un pitido y una explicación; el driver sigue rechazando los lazos.

### 0.β Preset de medición con referencia local (`Driver/src/avm_core.c`)

- `avm_matrix_default` (preset inicial del driver) pasa a ser el **preset de medición**:
  - Smaart Salida N → LISTENTO Envío N (1-8);
  - LISTENTO Retorno N → Smaart Entrada N (1-7);
  - **Smaart Salida 1 → Smaart Entrada 8**, **sin** el retorno 8 de LISTENTO.
- El preset anterior se conserva como `avm_matrix_direct` («Directo 8×8», `avmctl preset direct`).
- Constantes nuevas: `AVM_DST_LOCAL_REFERENCE` (Smaart Entrada 8) y `AVM_SRC_GENERATOR` (Smaart Salida 1).
- **Nombres:**
  - Driver: Smaart ve el canal 8 de entrada como «Entrada 8 (referencia local)» (nombre del elemento HAL).
  - `avm_dest_name`: «Smaart Entrada 8 (REFERENCIA LOCAL)».
  - `avmctl show` ensancha la columna de nombres.
- Las matrices guardadas en un Mac donde ya esté instalado se respetan; el preset solo se aplica si no hay matriz guardada o al pulsarlo.

### 0.γ Pruebas

| Prueba | Resultado |
| --- | --- |
| `test_core.c`: el aislamiento con el preset predeterminado ahora exige Entradas 1-7 = Retornos 1-7, **Entrada 8 = solo Smaart Salida 1** y Envíos = Salidas. Nueva `test_measurement_preset_cells`: una sola fuente en la referencia, sin el retorno 8, sin música, y el preset directo conserva el 8×8. `test_stale_data` fija explícitamente el preset directo, que es el que comprueba | **122/122** |
| `test_rate.c`, `test_install.sh`, `test_build_script.sh` | 115/115, 57/57, 7/7 |
| ASan/UBSan, TSan | Sin avisos |
| App (AppKit, ARC), driver y `avmctl`: compilación cruzada contra el SDK 15.5, arm64 + x86_64, `-Wall -Wextra -Werror` | Sin avisos |
| **Maqueta visual**: HTML con la misma geometría que `AVMMatrixView` (20 pt por casilla, 200 pt de nombres, 104 pt de cabecera), renderizado con Chromium | Imagen `AVMatrix_maqueta_matriz.png`, entregada aparte. **No es una captura del Mac** |

### 0.δ Pendiente en el Mac

- Ver la vista real de AppKit: el giro de los textos verticales, los colores en modo claro y oscuro, el ajuste de la ventana y el resaltado del ratón. Nada de eso se puede ejecutar en Linux.
- Confirmar con Antonio que la organización coincide con la de sus capturas de Dante.

## 0ter. Revisión 2 — correcciones pedidas por Codex sobre `9d17362`

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
