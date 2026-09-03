# CHANGELOG.md — Qué cambió y qué rompió

> Formato: fecha — qué cambió — qué se rompió — cómo se resolvió (o estado).
> **El propósito de este archivo es aprender de los regresiones.** Siempre anotar
> lo que ROMPIÓ, no solo lo que funcionó.

---

## 2026-09-03 (noche) — 4 mejoras a indi_mini (3 verificadas en vivo, pool pendiente de reboot)

**Cambió** (`utils/indi_mini.c`, binario `ade0eb16…` en `/usr/app/indi_mini`):
1. **FITS más completo:** tarjetas `XPIXSZ/YPIXSZ` (=2.9µm × bin, para
   plate-solving), `XBAYROFF/YBAYROFF=0` (offsets pares preservan fase RGGB),
   `XBINNING/YBINNING` reales. Selftest extendido. Verificado: download trae
   las tarjetas.
2. **Switch estándar `UPLOAD_MODE`** (Client/Local/Both, def + handler): Ekos
   ya no avisa "No UPLOAD_MODE... update driver". Verificado: set a Both y
   de vuelta a Client con echo correcto (quedó en Client).
3. **Binning 2x2 por soft** (promedio, helper `pix8` con selftest): def
   `CCD_BINNING` max 1→2, handler cuadrado 1/2, loop emite 960×540,
   `blob_sz` y header consistentes. Verificado: expo 0.05s en bin2 →
   FITS 1039680 B exactos (2880+960·540·2), NAXIS 960×540, XBINNING=2,
   XPIXSZ=5.80. Nota: el promediado mezcla el Bayer (para fotometría usar 1x1).
4. **Pool VB 4→8 bloques** (33MB, cabe en MMZ 64MB): **aplicado tras reboot**
   (ver abajo).

**Hallazgo (pool): `vb_exit` falla con `NOT_PERM` en restart.**
Decodificado con `ot_errno.h` del SDK: `0xa001800d` → mod VB, err `0x0d` =
`OT_ERR_NOT_PERM`. El `set_cfg` posterior retorna éxito pero es **no-op**
(igual que documenta AGENTS.md): el pool que vale es el del **primer run tras
el boot** (quedó en 4). Para que entre el 8 hace falta reboot. fix menor
incluido: pre-clean/teardown ahora loguean a stderr
(antes a stdout=/dev/null bajo daemon) + SIGTERM aborta worker en curso y
espera hasta 5s a que libere su frame antes del teardown.

**Resuelto con el reboot del usuario (2026-09-03 noche): pool=8 APLICADO**
(`blk_cnt 8, free 5, min_free 3`), `vb_fail_cnt=0` (antes 1/3 descartados),
`send≈int` (372/372). Nota: `frame_rate=1` post-boot NO es bug — Ekos hizo
una expo de 1s (`VMAX=0x9284=37508` por readback) y el VMAX queda donde lo
dejó la última exposición (diseño, sin restore); el sensor sigue el timing
pedido (1fps@37508, 20fps@1883, 30fps@1250). `i2c_peek` ahora en `/usr/app`
(persistente; `/tmp` se borra con reboot).

**Lección:** en este MPP, `vb_exit` tras `sys_exit` = NOT_PERM siempre; el
resize de pools VB solo entra con reboot. No reiniciar pools "porque sí":
para stills de Ekos el pool de 4 no molesta (20fps efectivos solo importan
para video).

## 2026-09-03 (sigue) — ROOT CAUSE downloading: `defBLOBVector perm="wo"` impedía que Ekos cree su BlobManager

**Síntoma:** Ekos siempre manda `enableBLOB Never`, el diálogo "would you like
to enable it?" + Yes no tiene ningún efecto en el wire, y el Preview queda en
"downloading" eterno. El `--blob-force` tampoco lo movió (Ekos descarta BLOBs
no pedidos en modo Never).

**Root cause (código fuente de Ekos, clonado y leído):**
- `ClientManager::newDevice` manda `B_NEVER` al conectar (por eso el Never).
- Los BLOBs los maneja una conexión SEPARADA `BlobManager` por propiedad BLOB,
  creada en `processNewProperty` SOLO si
  `prop.getType() == INDI_BLOB && prop.getPermission() != IP_WO`.
- Nuestro `defBLOBVector` anunciaba **`perm="wo"`** (¡write-only para IMÁGENES!)
  → Ekos **jamás crea el BlobManager** → `isBLOBEnabled()` siempre false →
  el Yes del diálogo llama a `setBLOBEnabled(true)` que no encuentra ningún
  manager y **no hace nada** → jamás llega `Also`/`Only` → downloading eterno.
- `setBLOBEnabled(true)` manda `B_ONLY` para (device, CCD1) — ya soportado
  (`blob_mode=2`, se envía igual que Also).

**Resuelto:** `perm="wo"` → `perm="ro"` (+ `state Ok`→`Idle` en el def, más
correcto). Binario `67d5c718…` desplegado (sin `--blob-force` en S96, ya
innecesario; el flag queda en el binario). Verificado: `perm="ro"` visible en
defs; flujo dos-conexiones (main Never + Only para CCD1) parsea `blob_mode=2`.

**CONFIRMADO por el usuario (2026-09-03 noche): el Preview de Ekos muestra la
imagen.** En el log se ve el BlobManager de Ekos (fd=12, `Only`, `blob_mode=2`)
y exposiciones completándose. Fin del ciclo connect→capture→download.

**Lección:** ante un cliente que "no pide", verificar qué espera el cliente
para pedir (perm del def), no solo el request. El `perm` mal puesto volvió
inútil todo lo demás (incluido el workaround force).

## 2026-09-03 (sigue) — Workaround `--blob-force`: Ekos 3.8.0 no manda Also aunque se le diga Yes

**Síntoma:** Ekos pregunta "image transfer is disabled, would you like to enable
it?", el usuario siempre dice Yes, pero en el wire **sigue mandando
`<enableBLOB>Never</enableBLOB>` en cada conexión** (verificado 4 conexiones
seguidas, `blob_mode=0` siempre). Sin BLOB, Ekos queda en "downloading" eterno
aunque la exposición complete Ok.

**Workaround en `utils/indi_mini.c`:** flag `--blob-force` (default OFF; activado
en `S96indi_mini`): el FITS se envía a todo cliente activo aunque haya pedido
Never. Si el cliente lo descarta, queda como antes (sin regresión); si lo
acepta, la imagen llega sin depender del ajuste de Ekos. Verificado por socket:
`Never` + force → BLOB 5.5MB + Ok en 0.7s. Binario `8bb02ba6…`, servicio con
`--blob-force`, device en `Idle`.

**Pendiente de confirmar:** si Ekos 3.8.0 MUESTRA el BLOB no solicitado o lo
descarta (entonces seguiría en "downloading" y habría que encontrar dónde Ekos
persiste el upload mode — jobs .esq, profile, o config file).

## 2026-09-03 (sigue) — "Stuck en capturing": sensor con vsync congelado, se recuperó con restart (causa raíz NO confirmada)

**Síntoma:** tras varios intentos de Ekos (exposiciones 1s + ABORTs), el worker
de exposición dejó de completarse: `exposición 1.000s` en log sin completion,
Ekos "capturing" eterno (en realidad loop fail→retry de ~15s por grab-timeouts).

**Evidencia (confusa):**
- `vsync_cnt` congelado (13→13 en 2s y 3s) con VMAX=37508 (período 1s) →
  parecía sensor detenido. PERO `freq_measure=896MHz`, `lane0_data=0xed`,
  `mipi_ph_d0=0x2c` (RAW12!), `vc0=1920x1080`, sin errores PHY → **el MIPI
  emite**. Contradictorio; el contador vsync quizá no refleja lo que creemos.
- VI pipe/chn `enable=Y`, `send_cnt=679`, `frame_rate=20`, pero
  `vb_fail_cnt=339` (1/3 de interrupciones descartadas: 1019 ints vs 679 sends).
- I2C OK durante el atasco (`i2c_peek`: 14/14 regs leídos, standby off,
  streaming on). El atasco murió con el restart: run nuevo entrega 0.05s y
  1s OK + BLOB 5.5MB.
- Tras el restart, exposición 1s tarda 2.5s reales → **el VMAX SÍ aplica**
  (los "0.5s" medidos antes fueron con VMAX ya aplicado y sin flush).

**Verificado en register map (`imx662_docs/`, SRM + Excel):**
- `0x3001` = REGHOLD (hold V-registers), `0x3002` = XMSTA (0=start/1=stop).
  El código tenía `#define REG_XMSTA 0x3001` (MAL rotulado; escribía REGHOLD=0,
  inocuo). Corregido a `0x3002` (la tabla init ya ponía `0x3002=0x00`, por eso
  siempre anduvo). VMAX/SHR/GAIN son reflexión "V" (sin standby, OK).
- Nueva tool `utils/i2c_peek.c` (solo lectura, I2C_RDWR) para diagnóstico en
  vivo sin tocar el pipeline. Binario en `/tmp` del device.
- Hardening: exposición solapada ahora re-envía `Busy` (antes solo mensaje →
  el cliente que reintentaba quedaba esperando a ciegas).

**Binario `f2269062…` desplegado, verificado** (0.05s/1s/2s + solapada → Ok,
device en `Idle`). Causa del wedge original: **no determinada** (candidatos:
stall I2C transitorio bajo martilleo exposición+ABORT, o estado VI/MIPI que
el restart limpió). Si recurre: `i2c_peek` + `vsync_cnt` + threads/wchan
antes de reiniciar.

**Lección:** `vsync_cnt` congelado NO implica sensor muerto si el PHY ve
clock+dato (mirar `freq_measure`/`lane0_data`/`mipi_ph_d0` primero).

## 2026-09-03 (sigue) — FIX: captura fallaba con Ekos en modo BLOB Never (Alert espurio)

**Síntoma:** Conectado OK, pero "Capture failed" 1-2s después de pedir 1s de
exposición, con reintentos y ABORTs encadenados.

**Root cause:** Ekos manda `<enableBLOB ...>Never</enableBLOB>` por defecto →
`blob_mode=0` → `stream_fits_blob` no tenía a quién enviarle el FITS
(`sent_ok=0`) → `return sent_ok ? 0 : -1` → **-1 aunque la captura había
salido bien** → worker responde `state="Alert"` + "frame capture failed".
Verificado por socket: con `Also` la misma exposición daba `Ok` + BLOB FITS
de 5.5MB en 0.5s (pipeline sano); el fallo era solo el modo Never.

**Resuelto:** `stream_fits_blob` devuelve 0 si la captura (grab+mmap) salió
bien, haya o no receptores BLOB; -1/-2 solo ante fallo real de captura o
abort. Binario `34a6a332…` desplegado, verificado: `Also` → Ok+5.5MB,
`Never` → Ok sin BLOB (344 B). Device en `Idle`.

**Lección:** según el estándar INDI, con BLOB Never el driver igual debe
completar la exposición con Ok y simplemente omitir el envío. No mezclar
"nadie pidió el blob" con "la captura falló".

## 2026-09-03 (sigue) — FIX: Ekos manda comillas simples → CONNECT ignorado (root cause del "Failed to connect")

**Síntoma:** Ekos conectaba al servidor (7624 OK, "Remote devices established")
pero "Failed to connect to IMX662 CCD" a los 10s. Por socket manual el CONNECT
respondía `Ok` en <1s → el protocolo parecía sano.

**Root cause (capturado en `/tmp/indi_mini.log` con el nuevo `--logfile`):**
Ekos manda XML con **comillas simples** y espacios de relleno:
`<newSwitchVector device='IMX662 CCD' name='CONNECTION'>  <oneSwitch
name='CONNECT'>      On  </oneSwitch></newSwitchVector>`. `xml_get_attr`
buscaba `device="` (dobles) → fallaba → el handler de CONNECTION hacía
`return` silencioso **sin responder** → Ekos agotaba su timeout de 10s.
(Doble bug: además el valor venía con espacios `"      On  "` y el
`strcmp(val,"On")` tampoco hubiera matcheado.)

**Resuelto en `utils/indi_mini.c`:** `xml_get_attr` acepta `'` o `"` como
delimitador; `xml_get_elem` recorta whitespace del valor. Selftest extendido
con el caso Ekos literal → `SELFTEST OK`. Binario `e2dc85f9…` desplegado en
`/usr/app/indi_mini`, servicio reiniciado, **verificado con los bytes exactos
de Ekos → responde `state="Ok"`**. Device dejado en `Idle` para la prueba.

**Nota:** Ekos además manda `<enableBLOB ...>Never</enableBLOB>` por defecto:
tras conectar hay que poner **Upload=Client/Both** en Ekos o no llegan BLOBs
(pendiente ya anotado). `S96indi_mini` ahora pasa `--logfile /tmp/indi_mini.log`.

**Lección:** el estándar INDI permite ambas comillas; un parser que solo acepta
dobles falla silenciosamente solo con ciertos clientes (probe Python OK, Ekos
roto). Ante "timeout del cliente", capturar los bytes reales antes de teorizar.

## 2026-09-03 (sigue) — Servicio S96indi_mini: `indi_mini` arranca solo al boot (VERIFICADO con reboot)

**Cambió:** Nuevo init script `general/overlay/etc/init.d/S96indi_mini`
(en el repo, para que sobreviva rebuilds) + instalado en vivo en el device
(`/etc/init.d/S96indi_mini`, overlay, 1517 B). Arranca
`/usr/app/indi_mini --preset validated --port 7624` con `start-stop-daemon`
(pidfile `/var/run/indi_mini.pid`, log `/tmp/indi_mini.log`). Orden S96:
corre después de S70vendor (módulos MPP ya cargados). El script hace
`modprobe open_adc` (NTC para CCD_TEMPERATURE; si falla arranca igual sin
temp — `open_adc.ko` ya vive en `/lib/modules`, no hay que copiarlo) y
detiene a majestic si estuviera corriendo (compite por el VI). Parada con
SIGTERM (limpia, nunca kill -9). Además se deshabilitó el autostart de
majestic (`chmod -x /etc/init.d/S95majestic`, persistente en overlay):
está descartado en este SoC (VI-online NOT_PERM) y corría antes (S95<S96),
riesgo de robarle el pipeline VI al arrancar.

**Verificado:** `start` → proceso + `LISTEN 7624` + handshake INDI
(`IMX662 CCD` responde `defTextVector`); ciclo `stop`/`start` OK (re-bindea
7624 sin estado VB corrupto); **reboot → a los ~45s el puerto 7624 ya
responde INDI** (uptime 0 min, pid 1088 = pidfile, majestic ausente).
Binario en `/usr/app/indi_mini` (md5 `58571d5b…`, 55K).

**Lección:** En este firmware el autostart no es crontab sino SysV
(`/etc/init.d/Sxx`, orden alfabético = orden de arranque). Para que un
servicio sobreviva tanto a reboots como a rebuilds hay que ponerlo en DOS
lados: `general/overlay/etc/init.d/` (repo) + `/etc/init.d/` en vivo
(overlay JFFS2, 300KB libres al momento).

## 2026-09-03 — NUEVO: utils/indi_mini.c (mini servidor INDI, puerto 7624)

**Cambió:** Servidor INDI mínimo en C puro, sin libindi (no cabe en flash/RAM).
Reutiliza el path RAW verificado de `astro_streamer.c` (VI ISP-bypass + init
manual + VMAX extendido para larga exposición). Expone device `IMX662 CCD` con
DRIVER_INFO, CONNECTION, CCD_INFO, CCD_EXPOSURE (0.001–25s, VMAX real en
sensor), CCD_ABORT_EXPOSURE, CCD_FRAME_TYPE, CCD_FRAME (crop por soft),
CCD_BINNING (solo 1x1), CCD_GAIN (dB 0–54 → again/dgain), CCD_TEMPERATURE y
CCD1 (BLOB FITS base64 en streaming, sin buffer de 5.5MB en RAM). Link mínimo
(ss_mpi/sysmem/sysbind/ot_osal/securec, sin ISP/VENC/VPSS). Binario ARM 51KB.

**Verificado:** cross-compila limpio (`build_indi_mini.sh`) + selftest de host
(`gcc -DINDI_SELFTEST`: base64, gain map, header FITS sin NULs, parser XML →
SELFTEST OK). **NO probado en device** (192.168.1.16 inalcanzable desde este
entorno): falta deploy + `indi_getprop`/Ekos contra 7624 + comparar FITS vs
golden_sets. Encontrado y corregido en el camino: `fits_card` dejaba NULs en el
bloque de 2880 (el mismo patrón sigue en `astro_streamer.c:write_fits` —
pendiente unificar).

**Lección:** El protocolo INDI es XML suficientemente simple como para hablarlo
a mano; la librería solo aporta drivers genéricos que acá no sirven (el sensor
necesita el pipeline propio).

## 2026-09-03 (sigue) — Temperatura del módulo: NTC en LSADC CH1 (TSENSOR descartado)

**Investigación (a pedido: "usar algún sensor del módulo como referencia"):**
1. **TSENSOR del SoC: DESCARTADO.** `pm.o` (open_pm) tiene `g_Tsensor_addr` y
   fórmula (`T=((165*raw-19305)/2*0xA4402911)>>32>>8-40`, raw10 en base+8),
   pero es para OTROS chips: `temp_ctrl_init` solo acepta IDs
   0x3516E200/0x3516E300/0x3518E200/0x3516D200 y mapea 0x1202xxxx. En CV610
   (SYS_CTRL en 0x11020000) leer 0x120200EE0 da **external abort (SIGBUS)**.
   Sin TRM del CV610 ni nodo thermal en el DTS → pozo sin fondo.
2. **Barrido SYS_CTRL+MISC bajo carga** (100% CPU 2.5 min): cero cambios →
   sin registro térmico visible ahí.
3. **LSADC (0x11100000): HALLAZGO.** `open_adc.ko` existe en el tree pero no se
   carga. Cargado a mano (`insmod /tmp/adc.ko` → `/dev/ot_lsadc`) + tool
   `utils/adc_read.c`: **CH1=841 estable** (10-bit), CH0=1023 (VREF),
   CH2/CH3=0 (GND). Bajo carga CH1 841→842 (reversible): **NTC térmico real**.
   I2C scan confirma que no hay otro chip de temp en el módulo.

**Implementado en `indi_mini` (binario `ecb8bd19`, corriendo):** lectura del NTC
por ioctl LSADC + Steinhart-Hart (`R=Rs*f/(1-f)`, defaults NTC 100k β=3950 +
serie 22k → 841 LSB ≈ 24.8°C, ajustables por `--temp-r-series/--temp-r25/--temp-beta`).
`CCD_TEMPERATURE` se anuncia SOLO si `/dev/ot_lsadc` abre bien (si no, como
antes: sin propiedad). Refresh post-exposición + tarjeta FITS CCD-TEMP.
**Verificado:** defs traen 24.80°C plausibles.

**Caveats:** el driver LSADC es open-exclusivo (segundo open da EPERM — por eso
`indi_mini` lo mantiene abierto); `open_adc.ko` vive en /tmp → **re-insmod en
cada reboot** antes de arrancar `indi_mini` (si no, arranca sin temperatura).
La calibración absoluta depende del divisor real: contrastar con termómetro
ambiente y ajustar `--temp-r-series` si difiere. Es temp de placa (cerca del
SoC), no del die del sensor: referencia relativa, no absoluta del pixel.

**Verificado en device (2026-09-03, segunda parte) — 4 bugs encontrados:**
1. Solo el 1er mensaje por conexión funcionaba: `xml_split_msg` incluye el `\n`
   previo en `ml` y el dispatch esperaba `msg[0]=='<'` → mismatch silencioso.
   Fix: strip de whitespace al extraer.
2. `setSwitchVector` de CONNECTION con `state`/`timestamp` trocados.
3. FITS en little-endian (el estándar exige big-endian) → Ekos/ds9 verían
   0xFF00 en vez de 0x00FF.
4. **Segfault en full-frame** (el subframe andaba): `b64_push` de una fila
   3840B emite 5120 chars en `out[4096]` → stack smash. Fix: `out[8192]`.
5. Frames "stale" tras cambiar VMAX (el pipe depth=1 entrega el frame integrado
   con el timing anterior): flush corto 500ms + 1 descarte si cambió el timing,
   captura directa si no. Sin esto, una expo de 1ms devolvía el frame saturado
   de la expo anterior de 1s.
6. ABORT imposible con worker bloqueante en el thread del cliente → exposición
   en worker thread dedicado (con generación por slot contra fd reuse).

**Estado final (binario `c3dd2615`, corriendo en device puerto 7624):**
1ms full → datos reales (0–255, mean ~124-151, unique=256, firma rain-noise);
2s → saturado 255 (esperado sin lente); subframe 960×540 OK; ABORT → Alert en
~0.1s sin BLOB; CCD_GAIN 0/30dB OK; MemFree ~34MB con todo corriendo.
FITS en `/tmp/opencode/indi_*.fits`. golden_sets/ no existe en este checkout
para comparar (pendiente).

## 2026-09-03 (sigue) — IMX662 SIN termómetro: se elimina CCD_TEMPERATURE

**Cambió:** El usuario preguntó por el sensor de temperatura. Verificado en
`imx662_docs/`: el register map (689 filas/modo) no tiene registros TEMP, el
SRM (24 págs) no menciona "temperature" y el datasheet solo da el rango
operativo (−30..+85°C, spec, sin readout). El `0x014A/B` que leíamos era
convención de otros Sony y devolvía 0x00 (0.00°C falso). Tampoco hay
`thermal_zone` del SoC en este kernel. Se eliminó la propiedad
CCD_TEMPERATURE de los defs, el update post-exposición y la tarjeta FITS
CCD-TEMP. Binario `9bbdbc40` corriendo en device (defs 3767→3513 B, resto
sin regresión: 1ms rain-noise OK).

## 2026-08-22 (noche) — FIX: CONFIG no conmutaba RAW→H265 (decoder bloqueado)

**Cambió:** Tras el fix anterior el viewer arrancaba bien en RAW, pero al apretar
CONFIG (RAW→H265) la pantalla seguía en RAW y no mostraba H265.

**Diagnóstico:** El streamer SÍ conmuta (verificado: reconnect post-kick recibe `WB`).
El problema era del **decoder thread** de `recv_astro.py`: al arrancar en RAW,
`cv2.VideoCapture(FIFO)` se abría sobre un FIFO vacío y quedaba **bloqueado en
`cap.read()`**; al pasar a H265 los frames se escribían al FIFO pero el decoder
nunca los leía → `DEC["frame"]` se quedaba en None → la pantalla no conmutaba.
Además `ctrl_send` devolvía silenciosamente si `CTRL` era None (canal de control
no conectado al inicio).

**Resuelto:**
- `_decoder_thread()` ahora solo decodifica cuando `READER["kind"]=="h265"`; en
  RAW libera el `cap` (no se bloquea en el FIFO vacío) y al volver a H265 lo
  **reabre limpio**. Esto garantiza que `DEC["frame"]` se actualice tras el switch.
- `ctrl_send()` auto-reconecta `CTRL` si es None (usa `CTRL_PORT` global), en vez
  de ignorar el comando. `main()` setea `CTRL_PORT` desde el 3er arg (default 5998).
- El HUD ya seguía `READER["kind"]` (fix previo), así que `mode=` y `line1` reflejan
  el modo real.

**Verificado:** protocolo switch RAW→H265 (fresh raw start) entrega `WB` tras
reconnect (binario `e97129a8`). Decoder fix es lógico (no testeable headless sin
display); requiere probar en el GUI del PC.

---

## 2026-08-22 (tarde) — FIX: switch H265↔RAW congelaba el viewer en H265

**Cambió:** El botón CONFIG/tecla 'm' de `recv_astro.py` no conmutaba el modo en
runtime: la pantalla seguía diciendo "H265" y la imagen no cambiaba.

**Diagnóstico:** El streamer (`astro_streamer`) SÍ conmuta (verificado: cliente nuevo
post-switch recibe `AS` en RAW / `WB` en H265). El bug era del **viewer + conexión
persistente**:
1. `read_frame()` (recv_astro.py:189) devuelve `None` ante cualquier magic de 2 bytes
   no reconocido → mata el reader thread. Durante un switch H265→RAW en la conexión
   persistente, el streamer puede estar a mitad de un frame H265; el viewer lee bytes
   desalineados → magic inválido → `None` → reader muere → la pantalla se congela en
   el último frame H265 ("stays h265, no change").
2. El `MODE` local del viewer arrancaba en `"raw"` (hardcode) mientras el streamer
   default es H265 → el primer `toggle_mode()` enviaba `MODE h265` (no-op) por desync.

**Resuelto:**
- `astro_streamer.c`: `switch_mode()` ahora setea `g_kick_client=1`; el data loop cierra
  el cliente de datos activo al completar el switch → fuerza reconnect limpio del viewer
  (evita el desalineo de parseo).
- `recv_astro.py`: `_reader_thread()` ahora **reconecta solo** si la conexión cae
  (bucle externo + `socket.create_connection` + backoff), en vez de morir.
- `toggle_mode()` ahora basa el modo en `READER["kind"]` (el modo REAL actual del
  stream), no en la variable privada desincronizada.
- `main()` ya no abre un socket de datos propio (ocupaba el único slot del server y
  bloqueaba el reconnect del reader); el reader usa los globals `HOST`/`PORT`.

**Verificado:** persist + switch H265→RAW → el cliente persistente recibe trailing H265
y luego FIN (kick); reconnect inmediato recibe `AS` (RAW). Build `e97129a8`.

**Lección:** nunca dejar que un parser de frames muera ante un magic inesperado en una
conexión persistente que puede cambiar de formato a mitad de stream; o se desconecta al
cambiar de modo (server kick) o se re-sincroniza (client reconnect).

---

## 2026-08-22 — GANANCIA RESUELTA + ctrl responde por socket

**Cambió:** Investigación a fondo del registro de ganancia IMX662 en `astro_streamer.c`.

**Hallazgos (verificados por readback I2C en device):**
1. **Registro de ganancia combinado** = `0x3070` (LSB) / `0x3071` (MSB, bits 10:8) =
   `GAIN[10:0]`, encoding `reg = dB*10/3` (paso 0.3 dB). `0x306C/0x306D` NO son gain
   (red herring del CHANGELOG 2026-08-17 — leídos dan 0).
2. **RAW (ISP bypass):** `A`/`D` escriben el sensor directo y FUNCIONA: `A 2048`→`0x14`
   (6 dB), `A 32768 D 16384`→`0xB5` (54 dB). Readback confirma.
3. **H265:** el registro `0x3070` lo conduce CONTINUAMENTE el ISP/AE — incluso un `W` crudo
   de `X`/`W` lo sobreescribe al instante (queda en `0x50` para escena saturada). Por eso
   los manuales `A`/`D` en H265 NO pegaban. AUTO = ganancia correcta e ISP-controlada.
   Manual en H265 no es fiable; para ganancia manual usar RAW.
4. **`gain_fixup_thread`** (H265+AUTO) añadido: lee `query_exposure_info`, calcula `reg`
   correcto y reescribe `0x3070/0x3071` cada 5ms cuando difiere del valor del ISP. Es un
   safety-net **redundante**: su `desired` siempre coincide con el `cur` del ISP (el lib
   `cmos_gains_update` codifica igual), así que no aporta pero no daña.

**Bug crítico encontrado y corregido — ctrl server:** las respuestas de `ctrl_handle_client`
(`X` readback, `R` status, ayuda) se escribían con `fprintf(stderr,...)`, NUNCA al socket
del cliente → todos los comandos devolvían respuesta vacía por TCP. Añadido `ctrl_send(cfd,...)`
y reenrutadas las respuestas del cliente al socket. Ahora `X`/`R`/`F` responden por TCP.
Añadido comando `W <reg> <val>` (escritura cruda de registro, para debug) y `F` (estado
del fixup: writes/desired/cur/ae_enabled).

**Cómo se diagnosticó el enredo previo:** el "gain no cambiaba con A/D" de 2026-08-17 era
porque (a) se miraba el registro equivocado (`0x306C`) y (b) en H265 el ISP pisa `0x3070`.
En RAW (el caso astro) la ganancia SIEMPRE funcionó.

**Binary:** `ff12974ee00539d852988a8ba41cb3e8` (astro_streamer, con ctrl_send + W/F + fixup).

---

## 2026-08-21 (tarde) — H265 ahora reporta exposición en el header (igual que RAW)

**Cambió:** Extendido el header WB de 12→**28 bytes** en `astro_streamer.c` (campos
`exp_us/again/dgain/vmax` rellenados con `ss_mpi_isp_query_exposure_info` en cada frame,
espejo del header AS RAW). `recv_astro.py` lo parsea: el HUD H265 ahora muestra
`E=…us A=… D=…` y la tarjeta FITS `EXPTIME` es correcta (antes `0`).

**Por qué:** la app mostraba "exposición 0" en H265 porque el header WB no traía `exp_us`;
los botones funcionaban pero no había feedback. Verificado: auto→27us, `T 300`→293us,
`T auto`→27us (valores reales, no 0).

**Binary:** `7510b3dcc8aa46d691ec2d63e203b4c3` (aún lleva logging de debug temporal en
sig_handler / data_loop ENTER-EXIT — limpiar antes de cerrar).

---

## 2026-08-21 — astro_streamer consolidado: RAW+H265 verificado FUNCIONAL (white = saturación, no bug)

**Cambió:** Conslidación de `astro_streamer` (modo RAW astrofoto, control de exposición/
ganancia, FITS, header AS-48B) en `astro_streamer.c`. Path RAW usa `init_sensor_full()`
(tablas portadas de astro) + `sensor_mclk_reset()`; path H265 usa el lib
(`sensor_setup`+`isp_setup`). Conmutación runtime `MODE raw|h265` por ctrl 5998.

**Hallazgos críticos (esta sesión, device power-cyclado por el usuario):**
1. **I2C estaba ENMASCARADO.** `i2c_read_reg` usaba write-then-read sin repeated-start
   → IMX662 NACKeaba las lecturas (devolvía 0x00 para todo, incluso chip ID). Y
   `i2c_write_reg` usaba `write()` que devuelve el byte-count sin importar el ACK →
   **falsos "149 ok"** (el sensor en realidad NACKeaba). Reescritos ambos con
   `I2C_RDWR` (repeated-start). Ahora los writes devuelven error en NACK y el readback
   es real (`30DC=32`, `3000=00`, `3014=03`, `3015=05`, `30B0=00`).
2. **El sensor REQUIERE `sensor_mclk_reset()` (reset clock-ON, réplica de
   `cmos_isp_init` del lib) para responder al I2C en este MPP.** `mipi_setup` (reset
   clock-OFF, idéntico a `enable_mclk_and_reset_sensor` de astro) por sí solo → el
   sensor NACKea TODOS los writes (0 ok 149 fail con I2C_RDWR). Con `sensor_mclk_reset`
   → 149 ok reales. Esto explica por qué astro "funcionaba" (su reset clock-off bastaba
   en el estado previo al power-cycle; tras el reboot el sensor solo ACKea con clock-ON).
3. **El frame blanco `f0 0f ff` NO es bug de MIPI/pipeline.** `/proc/umap/mipi_rx`
   confirma: `vsync_cnt` incrementando, `mipi_vc0_w/h=1920x1080`, `vc0_crc_err=0`,
   `freq_measure=896MHz`. El sensor EMITE frames MIPI válidos; el payload es `0xff`
   constante = pixel **0xFFF (saturado al máximo)**. Es **saturación del sensor sin
   lente en ambiente brillante** (documentado en CONTEXT 2026-08-10), no no-signal.
   El decode `f0 0f ff` → 0xFF es saturación, no la fill-pattern del VI.

**Se rompió / confundió:**
- `write_all()` con socket `O_NONBLOCK` (ctrl port) → writes grandes devolvían `EAGAIN`
  y cortaban el frame (ej: 47784 bytes). Arreglado con retry `select`+`EAGAIN`.
- `init_mod_common_pool(VI/VPSS/VENC)` movido ANTES de `mipi_setup` (parity astro) —
  ayuda al I2C pero el fix real fue I2C_RDWR + `sensor_mclk_reset`.
- H265 también daba frame blanco en sesiones previas: era **saturación** (mismo sensor),
  no artifact de decode ni bug de pipeline. El "éxito H265 2026-08-16" fue validar el
  pipeline (ffprobe/hevc 30fps), no la imagen; la imagen era saturada igual.

**Verificado en device (2026-08-21, post-reboot limpio):**
- `astro_streamer --preset validated --mode raw --bench 8` → I2C 149 ok 0 fail, sensor
  `30DC=32 3000=00 3014=03 3015=05 30B0=00`, ~30fps (179 frames/6s).
- TCP RAW: header "AS" 48B + 3110400 bytes/frame recibidos completos en PC.
- `/proc/umap/mipi_rx`: frames válidos, sin CRC errors → **pipeline FUNCIONAL**.
- Frame blanco = saturación (unique=1, mean=255). Cubrir el sensor / óptica → rain-noise
  (igual que golden_sets/imx662-lensless).

**Lección:** Nunca declarar "frame blanco = bug MIPI" sin leer `/proc/umap/mipi_rx`
(vsync_cnt + crc_err). Y el `i2c_read_reg` de IMX662 REQUIERE `I2C_RDWR` (repeated
start) o devuelve 0x00 (NACK). El reset clock-ON es obligatorio para I2C en este MPP
tras power-cycle.

---

## 2026-08-19 — astro_streamer modo H.265 VERIFICADO end-to-end + conmutación runtime raw↔h265

**Cambió:** `astro_streamer.c` soporta ahora RAW y H.265 con conmutación runtime por
canal de control 5998 (`MODE raw|h265` / botón CONFIG / tecla 'm' del viewer). Root
cause del VENC 0-frames = **doble init de sensor + falta de CSC de salida**.

**Se rompió (durante el trabajo):**
- **H.265 bench daba 0 frames** aunque el ISP corría (3A callbacks activos). Cause:
  `init_sensor_full()` en [6] dejaba el sensor streaming antes de que el lib lo
  reinicializara en `ss_mpi_isp_init` (doble init), + sin CSC de salida.
- **Backlog VENC (63-80fps en cliente):** `data_loop` sin cliente drenaba 1 frame/200ms,
  mucho más lento que la producción a 30fps → el cliente recibía ráfagas de backlog.
- **Decoder arrancaba mid-GOP** ("PPS id out of range") porque el primer frame enviado
  al conectar era un P-frame sin VPS/SPS/PPS.
- **SIGTERM no mataba el proceso** (musl SA_RESTART reiniciaba el `accept()` bloqueante).
- **Parser no aceptaba `--mode h265`** (solo `--mode=h265`).

**Resuelto:**
1. `init_sensor_full()` solo en MODE_RAW; en H.265 el lib hace la init (el override
   INCK/DATARATE se hace post-isp_init: standby→regs→unstandby→100ms→streaming).
2. CSC de salida (`ss_mpi_isp_get/set_csc_attr`, enable, satu=60, contr=53) en h265_isp_setup.
3. CCM del sensor tras arrancar el thread ISP (op_type AUTO, iso/temp_act FALSE).
4. VB pool[0] = `IMG_WIDTH*IMG_HEIGHT*2 + 0x4000` cnt 6 (era `RAW12_BUF_SIZE`).
5. Backlog: `data_loop` drena VENC en loop hasta vaciarlo cuando no hay cliente →
   **30.73 fps reales, pts=33333µs**.
6. Cliente nuevo en H.265: `ss_mpi_venc_request_idr(instant)` + esperar el primer IDR
   antes de enviar → primer frame = IDR, decoder sin errores.
7. `accept()` select-based (timeout 200ms) → SIGTERM limpio (teardown completo, sin panic).
8. Parser: `--mode h265` y `--mode=h265`; idem `--ctrl-port`/`--bitrate`.

**Verificado en device (2026-08-19):**
| Prueba | Resultado |
|--------|-----------|
| Bench H.265 | 300 frames @ 29.91 fps, 5 IDR, 26891 bytes |
| Bench RAW | 30 fps (sin regresión) |
| Entrega TCP H.265 | 30.73 fps, pts=33333µs |
| Decode en proceso (PC) | 284 frames, shape (1080,1920,3) — cv2 CAP_FFMPEG sobre FIFO |
| Conmutación runtime | h265→raw→h265 OK (frames fluyen en cada modo) |

**Lección:** En un pipeline VI+ISP+VENC, la init del sensor la debe hacer UNA sola
entidad (el lib del sensor durante `ss_mpi_isp_init`). Hacerla dos veces (una manual y
una del lib) deja el sensor en un estado de streaming que el ISP no reconoce → VENC 0
frames. Y el VENC NO se puede "leer y descartar" 1 vez cada 200ms si produce a 30fps:
hay que drenarlo hasta vaciarlo para evitar backlog.

---

## 2026-08-18 — EXPERIMENTO MAJESTIC CANCELADO definitivamente (root cause confirmada en disassembly)

**Cambió:** Se intentó revivir majestic (experimento LD_PRELOAD). Se desensambló
`hi_sys.o` (driver sys), `libss_mpi.so` y el binario majestic. **Conclusión: no merece
la pena. Root cause definitiva:**

- **Majestic SIEMPRE llama `ss_mpi_sys_set_vi_vpss_mode`** (PLT 0xa4d8, llamada en
  majestic 0x2f34a) con modo calculado en 0x2f2fc-0x2f322: `r2 = [config+0x479] ^ 1`;
  si width ≤ 3200 → `mode[0] = r2+2` (para 1920x1080 → **VI_ONLINE**, modo 2 o 3).
- **El kernel rechaza el cambio con 0xa002800d (NOT_PERM)**: wrapper `sys_ioctl_set_vi_vpss_mode`
  (hi_sys.o 0x235c) lee el flag `[0x660+0x4bc]` (puesto a 2 en `sys_do_mod_init` 0x3a7c);
  si ≠ 0 → NOT_PERM sin llamar al driver. `sys_ioctl_user_init` (0x2914) solo lo resetea
  a 0 tras `cmpi_init_modules` con flag==2.
- El driver `sys_drv_set_vi_vpss_mode` (0x4358) además exige: modos 1/3 → module VPSS
  registrado (`cmpi_get_module_func_by_id(7)`), y que NINGUNA pipe VI (0..3) exista
  (`[vi_func+8]`=`pfn_vi_is_pipe_existed`) ni grp VPSS (0..5). VI-online es atributo
  estático fijado a OFFLINE en este MPP (ver experimento 2026-08-16).
- majestic NO aborta al fallar (loguea error línea 0x603 y reintenta en loop 0x2f2d8),
  pero su pipeline queda configurado VI-online contra un MPP offline-only → jamás dará
  frames (era la causa del VENC timeout histórico).

**Decisión:** NO hacer el experimento LD_PRELOAD para forzar offline en majestic. El
pipeline que funciona y es mantenible es **astro_streamer (VI-offline + ISP + VPSS + VENC
H.265 a 30fps)**. majestic queda descartado para siempre en este SoC.

**Lección:** Antes de revivir un binario cerrado, verificar en disassembly si el modo
de pipeline (online vs offline) es configurable. Este MPP es offline-only de fábrica.

---

## 2026-08-17 — MEMORIA REDIMENSIONADA (128MB DDR completo) + control de exposición verificado end-to-end

**Cambió:** El módulo tiene 128MB DDR pero estaba configurado como 64MB (`totalmem=64M`)
→ Linux 32MB + MMZ 32MB + 64MB sin mapear. Se corrigió U-Boot env (persistente, sin rebuild):
`fw_setenv totalmem 128M` y bootargs `mem=64M` → `load_hisilicon` calcula MMZ = 128−64 = **64M**.

| Región | Antes | Después |
|--------|-------|---------|
| Linux | 32MB (MemTotal 25MB, ~6MB libres, OOM-tight) | 64MB (MemTotal ~57MB, MemFree ~35MB) |
| MMZ | 32MB (pools default 29MB + VENC recon NO cabían) | 64MB (default pools + VENC caben) |
| Sin mapear | 64MB | 0 |

**VERIFICADO en device:** bench `astro_streamer --offline --preset validated --bench 10`
con **pools default (sin `--raw-blk/--yuv-blk`)**: init todo OK (sin `ILLEGAL_PARAM`),
**300 frames @ 29.9fps**, teardown limpio (`sys_exit=0x0`). Ya NO hace falta reducir pools.

**También se verificó el control de exposición end-to-end** (comando `X <reghex>` de
readback I2C añadido al canal de control de astro_streamer):
- `E 300` → SHR (0x3050) = **0x0B** (11 líneas ≈293µs) ✓
- `E 30000` → SHR = **0x0465** (1125 líneas ≈30ms) ✓
- El registro del sensor cambia correctamente con la orden → el canal AE (MANUAL) funciona
  de punta a punta (comando → ISP AE → sensor). La ganancia (0x306C/0x3070) no cambió
  (a investigar el mapeo de registros del lib si hace falta).

**Se rompió (consecuencia colateral):** `load_hisilicon -a -s imx662` (reload de módulos)
**cuelga el kernel 2 veces seguidas** (watchdog reinicia). No usar reload como recovery;
usar reboot directo.

**Lección:** dimensionar siempre contra el DDR REAL del board (`totalmem` env), no el default
de 64M. La memoria del MMZ se calcula en cada boot desde `totalmem - mem` en `load_hisilicon`.

---

## 2026-08-16 — EXPERIMENTO waybeam-venc: pipeline completo VERIFICADO en device (30fps H.265)

**Cambió:** Branch `experiment/waybeam-venc`. Se portó el pipeline de waybeam (mismo
módulo SIP-K662C6S): MIPI completo con `ENABLE_MIPI_CLOCK`, VI + ISP 3A (dlopen
`libsns_imx662.so`), VPSS escalador, VENC H.265 CBR, salida TCP (header "WB"). Se añadió
el knob de clock `sns0_clk_hz` al `open_sys_config.ko` real (`hi3516cv6xx/sys_cfg.c`,
CRG 0x8440). **TODO VERIFICADO EN DEVICE** (2026-08-16, sesión SSH 192.168.1.16).

**Resultados medidos en device (VI-offline):**

| Config | MCLK | FPS | Nota |
|--------|------|-----|------|
| 1 lane validated (INCK=0x03/DR=0x05) | 27 MHz | **30.0** | 300 frames/10s bench |
| 4 lanes waybeam (INCK=0x01/DR=0x03/LANE=0x03) | 27 MHz (knob ausente) | 21.9 | ratio 27/37.125×30 ⇒ MCLK real 27MHz |
| **4 lanes waybeam + knob** | **37.125 MHz** | **30.0** | knob escribió 0x8010 (32784); readback sensor OK |

- **VENC H.265 funciona**: ffprobe decodifica la captura TCP → `hevc Main 1920x1080 30/1`.
  El timeout de majestic NO era del encoder (era VI-online + ISP).
- **Los 4 lanes están cableados** en el módulo (streaming OK a 4 lanes).
- **El knob `sns0_clk_hz` funciona**: 0x8010 produce 37.125MHz reales (fps 21.9→30.0).
- Streaming TCP end-to-end: header "WB" 12B + AnnexB, 408 start codes, VPS/SPS/PPS/IDR OK,
  SIGTERM limpio (teardown completo, sin panic).

**Hallazgo clave (VI-online NO soportado):** `ss_mpi_sys_set_vi_vpss_mode` falla con
`0xa002800d = OT_ERR_NOT_PERM` ("cambiar atributo estático"). El modo VI-online es un
**atributo estático fijado a OFFLINE** en este MPP — no se puede cambiar en runtime.
**Esto explica por qué majestic nunca funcionó** (VI-online + VENC timeouts). La solución
es **VI-offline** (VI→VB→VPSS→VENC), que funciona perfecto a 30fps con ISP 3A activo.

**Bugs de build descubiertos durante el deploy:**
- `libsvp_acl.so` y `libaiisp.so` del firmware son prebuilts ROTOS (símbolos protobuf-c/
  svp_acl undefined — nunca usados por nadie). Sacados del link. `libot_mpi_isp.so` SÍ
  necesita los blobs AI (bnr/drc/ldci/etc) → se mantienen.
- musl `signal()` usa SA_RESTART → SIGTERM no interrumpe `accept()` (proceso colgaba en
  TCP wait). Arreglado con select-based accept + check de `g_stop`.
- `--bench N` no desactivaba el TCP (quedaba bloqueado en accept). Ahora bench puro.

**Se rompió (falsa alarma):** un intento de streaming sin `--offline` falló al arranque
(online NOT_PERM). No es regresión — es la limitación de VI-online documentada arriba.

**Entregables actualizados:**
- `utils/astro_streamer.c` + binario (md5 a5dc3ace87f1f3ed783f71b9c35a23ee) — compilado SIN
  libaiisp/libsvp_acl (link mínimo: ss_mpi, isp, ae, awb, ot_mpi_isp, blobs AI, sysmem,
  sysbind, ot_osal, securec).
- `utils/deploy_astro_streamer.py` — deploy binario + lib completa + .ko con knob.
- `docs/parches/PATCH_SNS0_CLK_HZ.md` + `.patch` — knob kernel persistente (VERIFICADO).
- Knob desplegado en device: `/sys/module/open_sys_config/parameters/sns0_clk_hz`
  (backup del .ko original en `/tmp/open_sys_config.ko.bak`).

---

## 2026-08-10 (noche) — BUG BLANCO RESUELTO: era saturación sin lente, no bug MIPI RX

**Cambió:** Se descartó definitivamente el "bug del frame blanco tras reboot" como
bug del MIPI RX. Con el sensor tapado (bloqueando la luz) los frames cambian:
`unique=3→130`, `diff_bytes=0→261054` ⇒ el sensor entrega datos reales.

**Evidencia:**
- MIPI RX parsea paquetes reales: `freq_measure=896MHz`, `mipi_ph_d0=0x01` (Frame Start),
  `mipi_vc0_w/h=1920x1080`, `vsync_cnt=10`, solo 2 CRC errors
- VI entrega frames a 30fps: `int_cnt=55706, send_cnt=55705, 0 lost, 0 vb_fail`
- `recv_astro.py` con botones AGAIN/DGAIN/EXP muestra imagen real en PC

**Se rompió (falsa alarma):** Frame 100% blanco estático `f0 0f ff` que no respondía
a exposición/ganancia. La causa NO era el MIPI RX sino **saturación del sensor sin
lente** (capta toda la luz del entorno a exp 33040us + ganancia máxima).

**Resuelto:** No requería fix de software. Es comportamiento de hardware esperado.
El pipeline MIPI RX→VI→TCP está funcional y verificado.

**Lección:** Antes de declarar un bug de MIPI/pipeline, descartar saturación del
sensor sin óptica. Diagnóstico rápido: `python3 /tmp/opencode/recv_test.py`;
si `unique=3, diff=0` → tapar el sensor y verificar que `unique` suba.

---

## 2026-08-10 — Sesión: frame blanco tras reboot (SIN RESOLVER)

**Cambió:** Reboot del SoC. Antes (9-Ago 20:15-20:55) el streamer producía ruido-lluvia correcto.

**Se rompió:** El frame es 100% blanco estático `f0 0f ff` (unique=3, diff=0 entre frames).
No responde a exposición (E=1239 vs E=11) ni ganancia (A=1024 vs A=32768).

**Intentado:**
- `load_hisilicon -a -s imx662` (reload completo de módulos MPP) → init pasa completo pero frame sigue blanco
- `rmmod open_mipi_rx` + `insmod` → streamer fallaba `set_dev_attr 0xa0108011` (estado inconsistente), resuelto con reload completo
- Readback de registros del sensor → todo correcto (chip ID 0x32, INCK=0x03, DR=0x05, test pattern OFF)

**Diagnóstico:** MIPI RX no recibe datos válidos del sensor. El patrón `f0 0f ff` es la
firma del VI sin señal MIPI. **Siguiente paso: power cycle físico del sensor** (cortar VCC),
único paso no probado.

**Lección:** El reload de módulos NO equivale a power cycle del sensor. El binario era
idéntico (md5 e63c5428), la diferencia fue el estado de hardware tras reboot.

---

## 2026-08-09 (noche) — astro_streamer produce imágenes correctas (ruido-lluvia)

**Cambió:** Binario `astro_streamer` compilado (20:12), imágenes capturadas 20:15-20:55.
`/root/astro_streamer` copiado a las 23:12 (binario persistente, md5 e63c5428).

**Funcionó:** Frames con ruido temporal (mean ~140, frame-diff=99) = correcto para sensor sin lente.
PNGs en `/tmp/opencode/astro/` → movidos a `golden_sets/imx662-lensless/`.

**Nota crítica:** Aunque AGENTS.md en su momento lo marcó como "imagen sin correlación espacial",
el usuario aclaró que el ruido-lluvia es el comportamiento CORRECTO (el sensor no tiene lente).

---

## 2026-08-09 — Descubrimiento: vc_num=0 + common pool = 30fps exactos

**Cambió:** `vi_raw_capture.c` reconstruido pasó de funcionar a fallar `get_chn_frame 0xa0108016`
tras usar `create_pool()` (pool usuario, 4 bloques) + vc_num=1.

**Se rompió:** `vi_raw_capture` dejó de entregar frames.

**Resuelto:** `astro_streamer.c` usa **common pool** (`get_common_pool_id()` con 6 bloques,
sin `create_pool()`) + vc_num=0 → 30.0fps estables. La causa no era el vc_num sino el
**tipo de pool VB**.

---

## 2026-08-08 — Sweep INCK_SEL/DATARATE_SEL

**Cambió:** Se barrió INCK_SEL (0x3014) y DATARATE_SEL (0x3015) relockeando el PLL en runtime.

**Resultado:** INCK_SEL=0x03 (27MHz) + DATARATE_SEL=0x05 (891Mbps) = **30fps exactos**.
Otras combinaciones daban 9.7-34.7fps (mal).

**Se rompió en el camino:** La tabla estática `imx662_init_common`/`imx662_init_mode` usaba
INCK=0x01/DR=0x02 (22fps). Se agregó override post-standby-exit con defaults correctos
+ flags `--incksel`/`--datarate`/`--sweep`.

---

## 2026-08-07/08 — Majestic abandonado (decisión 0001)

**Cambió:** Se probó majestic a fondo (LD_PRELOAD fix_pixel_rate, INI params, etc.).

**Se rompió:** VENC timeout (106+), exposure stuck 125ms (8fps), OOM kill (25MB RAM).
`mipirx not set lane mode` en dmesg (2x en boot).

**Resuelto (decisión):** Abandonar majestic (closed source, Prosperity License), usar
streamer propio con ISP bypass. Ver `docs/decisions/0001-abandonar-majestic.md`.

---

## 2026-08-06/07 — Config sensor corregida

**Cambió:** HMAX byte order (LOW first → 1980 correcto vs 48135 WRONG), INCK_SEL, DATARATE_SEL,
WINMODE (HD1080 crop 0x04), crop regs, 0x3A50-52 (12-bit output), cmos_restart DATARATE_891,
imx662_restart_mode, sensor reset en enable_mclk.

**Se rompió (y se arregló):**
- INCK mal → 22fps. Fijado a 27MHz (0x03) = 30fps
- HMAX byte order invertido → VMAX/imagen corrupta. Corregido
- `vi_raw_capture` OOM por escribir frames a /tmp → streaming TCP sin disco

---

## 2026-08-05 — Primeros pasos

**Cambió:** Descubrimiento del bus I2C (solo bus 0), MCLK+reset necesario para I2C
(secuencia de 3 pasos: ENABLE_SENSOR_CLOCK → RESET → 10ms → UNRESET → 100ms).
Chip ID 0x32 leído por primera vez.

**Se rompió:** Sin MCLK+reset el sensor hacía NACK (`bsp-i2c: wait idle abort! RIS: 0x611`).

---

## Formato de entrada nueva

```md
## YYYY-MM-DD — Resumen corto

**Cambió:** ¿Qué se tocó?

**Se rompió:** ¿Qué dejó de funcionar (si aplica)?

**Resuelto / Intentado:** ¿Cómo se arregló, o en qué quedó?

**Lección:** ¿Qué se aprendió para no repetirlo?
```
