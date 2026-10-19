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

## 6. Prueba en Smaart: enviar y recibir a la vez

1. Prepara un bucle interno, solo para esta prueba (Salida 1 → Entrada 1):
   ```bash
   avmctl preset clear
   avmctl connect smaart.out.1 smaart.in.1
   avmctl show
   ```
2. Abre **Smaart**. Si pide permiso de micrófono, acéptalo.
3. En la configuración de audio de Smaart (I/O Config), elige **AV Matrix Smaart** como dispositivo de **entrada** y de **salida**.
4. Generador: **ruido rosa** por la **Salida 1** de AV Matrix Smaart. Empieza con un nivel bajo.
5. Medición: la **Entrada 1** de AV Matrix Smaart. Debe verse el ruido rosa en el medidor y en el espectro.
6. Opcional: función de transferencia con referencia = Entrada 1 y medición = Entrada 1. Debe salir plana y coherente; sirve para ver que la señal llega intacta.

Anota lo que observes:

- [ ] ¿Aparece «AV Matrix Smaart» en la lista de dispositivos de Smaart?
- [ ] ¿Se puede elegir como entrada y como salida a la vez?
- [ ] ¿El ruido rosa enviado por la Salida 1 llega a la Entrada 1?
- [ ] ¿Las Entradas 2-8 quedan en silencio?
- [ ] ¿Hay cortes o chasquidos?

## 7. Al terminar

Vuelve a la configuración normal (Smaart → LISTENTO y LISTENTO → Smaart):
```bash
avmctl preset default
```
También puedes abrir la app **AV Matrix** (en Aplicaciones) para ver y cambiar la matriz.

## Desinstalar

Cierra las apps de audio (el audio se corta unos segundos) y ejecuta:
```bash
sudo bash scripts/uninstall.sh
```
Se guarda una copia de lo eliminado en `~/AVMatrix-backups/uninstall_<fecha>/`.
