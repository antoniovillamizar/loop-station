# AV Matrix — instalación y primera prueba con Smaart

Esta primera prueba solo comprueba que **Smaart reconoce «AV Matrix Smaart»** y que puede **enviar y recibir audio a la vez**. Todavía no se usa LISTENTO ni se prueba con Colombia.

Todas las órdenes se escriben en la app **Terminal** (Aplicaciones → Utilidades → Terminal). `sudo` pide la contraseña de tu usuario del Mac. Al escribirla no se ve nada; es normal.

## 1. Preparar

1. Descomprime `AVMatrix-….zip`. Doble clic en Descargas crea la carpeta `AVMatrix`.
2. En Terminal, entra en esa carpeta:
   ```bash
   cd ~/Downloads/AVMatrix
   ```
3. Mira si tienes las herramientas de compilación de Apple:
   ```bash
   xcode-select -p
   ```
   - Si muestra una ruta (por ejemplo `/Library/Developer/CommandLineTools`), sigue la **opción A**.
   - Si da error, usa la **opción B**, o instala las herramientas con `xcode-select --install` (tarda unos minutos) y vuelve a la opción A.

## 2. Comprobar sin instalar nada

**Opción A (recomendada): compilar en tu Mac**
```bash
bash scripts/build.sh
sudo bash scripts/install.sh --dry-run
```

**Opción B: usar la versión precompilada** (incluida en `dist/`)
```bash
sudo bash scripts/install.sh --dry-run --sign-identity=-
```

Al final debe decir **«Prueba en seco correcta»**. Si sale **ERROR**, para aquí y envía lo que aparece en pantalla.

**Icono:** con la opción A, `build.sh` toma el icono de la app que ya tienes instalada (o de `App/Resources/AVMatrix.icns`) y lo incluye; debe aparecer «Icono importado…» o «Icono: …». Con la opción B, el instalador conserva el icono instalado y debe aparecer «se conserva el icono instalado».

## 3. ⚠️ Antes de instalar: cierra Smaart, LISTENTO y Apple Music

La instalación reinicia el servicio de audio de macOS (`coreaudiod`), y **todo el audio del Mac se corta unos segundos**. Cierra también Spotify, Tidal, tu DAW y cualquier otra app de audio.

## 4. Instalar

- Opción A: `sudo bash scripts/install.sh`
- Opción B: `sudo bash scripts/install.sh --sign-identity=-`

Al final debe aparecer **«AV Matrix instalado y operativo»** y la lista de los tres dispositivos.

- Si había una versión anterior, queda una copia en `~/AVMatrix-backups/<fecha>/`.
- **Si algo falla, el instalador deshace la instalación solo** y muestra ERROR. En ese caso, envía lo que salió en pantalla y también el resultado de:
  ```bash
  log show --last 5m --predicate 'process CONTAINS "coreaudiod" OR process CONTAINS "Core Audio Driver"' | grep -i avmatrix
  ```

## 5. Autoprueba automática (con todas las apps de audio cerradas)

```bash
avmctl test
```

- macOS puede pedir permiso de **micrófono para Terminal**. Acéptalo; es necesario aunque las entradas sean virtuales.
- Debe terminar con **«RESULTADO: CORRECTO»**. Al final restaura la matriz.
- También comprueba los **medidores** del driver con los tres dispositivos abiertos: nivel exacto de cada canal, separación entre canales, una entrada que suma dos fuentes, el indicador de saturación y que todos pasan a inactivo al detener el audio.

## 6. Prueba en Smaart: enviar y recibir a la vez

1. Carga el preset de medición:
   ```bash
   avmctl preset default
   avmctl show
   ```
   Smaart Salida 1 llega a **Smaart Entrada 8 (referencia local)**. Las entradas 1-7 esperan los retornos de LISTENTO; con LISTENTO cerrado quedan en silencio.
2. Abre la app **AV Matrix** (en Aplicaciones). Comprueba en la matriz la casilla verde ✓ en el cruce de la columna **Smaart → Salida 1** con la fila **Smaart → 8 · Entrada 8**.
3. Abre **Smaart**. Si pide permiso de micrófono, acéptalo.
4. En la configuración de audio de Smaart (I/O Config), elige **AV Matrix Smaart** como dispositivo de **entrada** y de **salida**. El canal 8 debe aparecer como «Entrada 8 (referencia local)».
5. Generador: **ruido rosa** por la **Salida 1** de AV Matrix Smaart. Empieza con un nivel bajo.
6. Mira la **Entrada 8**: debe verse el ruido rosa en el medidor y en el espectro.
7. Opcional: función de transferencia con referencia = Entrada 8 y medición = Entrada 8. Debe salir plana y coherente.

Anota lo que observes:

- [ ] ¿Aparece «AV Matrix Smaart» en la lista de dispositivos de Smaart?
- [ ] ¿Se puede elegir como entrada y como salida a la vez?
- [ ] ¿El ruido rosa enviado por la Salida 1 llega a la Entrada 8?
- [ ] ¿Las Entradas 1-7 quedan en silencio?
- [ ] ¿Hay cortes o chasquidos?
- [ ] ¿La matriz de la app se entiende como la de Dante?
- [ ] Medidores: con el ruido rosa en marcha, ¿**Smaart Salida 1** (arriba) y **Entrada 8** (izquierda) marcan el mismo nivel en dBFS que indica Smaart?
- [ ] ¿Las Entradas 1-7 muestran «—» con LISTENTO cerrado?
- [ ] Al subir el generador a 0 dBFS, ¿se enciende el LED rojo de saturación en Salida 1 y Entrada 8, y se apaga unos 2 s después de bajarlo?
- [ ] Al detener el generador en Smaart, ¿los medidores pasan a «—» en menos de medio segundo?
- [ ] Al cerrar y volver a abrir la app AV Matrix, ¿Smaart sigue recibiendo sin cortes?
- [ ] Nombres: doble clic sobre «Salida 1» y escribe «Generador rosa» + Enter. ¿Aparece «1 · Generador rosa» arriba y en la descripción de la ruta, con las conexiones y el nivel iguales?
- [ ] Doble clic sobre «Entrada 8», escribe «Señal de referencia» y pulsa Escape: ¿no cambia? Repite con Enter: ¿aparece «8 · Señal de referencia» y, al dejar el ratón encima, el nombre completo?
- [ ] Clic derecho sobre el nombre → «Restaurar nombre original»: ¿vuelve el nombre de fábrica?
- [ ] Cierra y abre la app: ¿se conservan los nombres con sus tildes?
- [ ] Pantalla compacta: ¿se leen número, nombre, barra, dBFS y saturación de cada receptor sin solaparse, y la Entrada 8 sin rótulo naranja?
- [ ] Botón «Ayuda» y menú Ayuda → Manual de AV Matrix: ¿se abre el manual? ¿Lleva cada entrada del índice a su sección? ¿Se puede agrandar y achicar la ventana?

## 7. Al terminar

La configuración queda en el preset de medición, que es la que se usará con LISTENTO más adelante. No hace falta cambiar nada.

## Actualizar solo la app (sin tocar el driver ni cortar el audio)

Cuando una versión solo cambia la app (como la revisión 9, con el panel de nombres corregido):

1. Cierra la app **AV Matrix** (no hace falta cerrar Smaart ni LISTENTO: el audio no se corta).
2. Compila y comprueba:
   ```bash
   bash scripts/build.sh
   sudo bash scripts/install.sh --app-only --dry-run
   ```
   Durante `build.sh`, el panel de nombres se abre y se cierra solo varias veces en pantalla (prueba nativa). Debe terminar con «Panel de nombres: … 0 fallos».
3. Instala: `sudo bash scripts/install.sh --app-only` (con la versión precompilada: añade `--sign-identity=-`).

Debe terminar con **«App actualizada. No se tocó el driver ni avmctl y no se reinició coreaudiod.»** La app anterior queda en `~/AVMatrix-backups/<fecha>/`. Los nombres de los canales y el icono se conservan.

## Desinstalar

Cierra las apps de audio (el audio se corta unos segundos) y ejecuta:
```bash
sudo bash scripts/uninstall.sh
```
Se guarda una copia de lo eliminado en `~/AVMatrix-backups/uninstall_<fecha>/`.
