# AGENTS.md - Estado del Proyecto IMX662 + Hi3516CV610 (OpenIPC)

> **Este archivo es la referencia técnica de trabajo.** El proyecto usa una estructura
> de documentación viva (2026-08-10):
> - **`CONTEXT.md`** → estado ACTUAL (pipeline VERIFICADO, qué funciona, cómo diagnosticar)
> - **`CHANGELOG.md`** → historial de qué cambió y qué se rompió (CRÍTICO)
> - **`docs/decisions/`** → decisiones de arquitectura (ADRs) y por qué
> - **`docs/architecture.md`** → pipeline, frame format, config sensor
> - **`tests/checklist.md`** → qué probar antes de declarar que algo funciona
> - **`golden_sets/`** → ejemplos del comportamiento correcto y bugs conocidos
> - **`tools/prompts/`**, **`tools/skills/`** → prompts y skills reutilizables
>
> **Regla de sesión:** al terminar, actualizar `CONTEXT.md` + `CHANGELOG.md`.
> Nunca declarar "funciona" sin comparar contra `golden_sets/`.

## Resumen

**Sensor:** Sony IMX662, 1 lane MIPI, 1080p30 raw12, i2c-0 addr 0x34
**SoC:** Hi3516CV610, ~25MB usable RAM, OpenIPC
**Estado:** ✅ 30fps CONFIRMADO (bench + sweep). INCK_SEL=0x03 (27MHz) + DATARATE_SEL=0x05 (891Mbps). **DECISIÓN (2026-08-09): abandonar majestic, usar streamer propio con ISP bypass**. **astro_streamer es EL streamer que funciona (30fps exactos, frames confirmados por TCP puerto 5999 + recv_astro.py).** vc_num=0 + common pool (no create_pool). Config sensor validada contra 8 repos GitHub.

**FORMATO PAYLOAD (2026-08-10): LSB-first 12-bit, datos 8-bit desplazados <<4.**
Análisis de estructura de bytes (triplet `[b0,b1,b2]` por 2 píxeles) probó:
- `b0` siempre múltiplo de 16 (nibble bajo = 0), `b1` siempre <16 (nibble alto = 0), `b2` rango completo
- ⇒ el sensor entrega datos 8-bit (`v<<4`) en contenedor 12-bit **LSB-first**
- Decode correcto: `v0 = ((b1&0xF)<<4)|(b0>>4)`, `v1 = b2` (equivalente: 12-bit LSB >> 4)
- `recv_astro.py:raw12_to_uint16` y `recv_raw.py:unpack_raw12` FIJADOS (antes MSB-first, WRONG)
- ⚠️ Sin embargo la imagen NO muestra correlación espacial (ch2/cv2 < 0.4 en TODOS los decodes):
  mean 140/255 a exposición mínima (293us, ISO 100), solo 18 valores únicos en 2MP, frame-diff=99.
  Campo plano brillante + ruido temporal (bits bajos ruido). Escena aparentemente sobreexpuesta/
  flat, o el sensor no entrega imagen coherente. Los PNG de prueba están en /tmp/opencode/astro/.

---

## Hardware

| Componente | Detalle |
|------------|---------|
| Sensor | Sony IMX662, chip ID 0x32 |
| I2C | Bus 0 SOLO (buses 1 y 2 NO cableados), addr 7-bit 0x1a (8-bit 0x34) |
| MIPI | 1 lane físico, `/dev/ot_mipi_rx`, módulo `open_mipi_rx` |
| GPIO I2C0 | SDA=GPIO6_6 (IOCFG2 `0x17940098`, mux=0x1135), SCL=GPIO6_7 (IOCFG2 `0x1794009C`, mux=0x1135) |
| RAM | 32MB total (`mem=32M` en cmdline) |
| Flash | rootfs SquashFS 8MB + appfs 4.2MB + rootfs_data (overlay JFFS2) 512KB |
| Red | Ethernet 100Mbps, IP 192.168.1.16 |
| Conexión | SSH paramiko (root/12345), SFTP roto, upload por base64 chunked |

### Particiones flash

| MTD | Nombre | Tamaño | Uso |
|-----|--------|--------|-----|
| mtd0 | boot | 320KB | U-Boot |
| mtd1 | kernel | 3MB | Linux |
| mtd2 | rootfs | 8MB | SquashFS (read-only) |
| mtd3 | appfs | 4.2MB | Aplicaciones |
| mtd4 | rootfs_data | 512KB | JFFS2 overlay (75% usado) |

### Memoria (32MB total)

El sistema es MUY limitado en RAM. Consideraciones:
- `/tmp` es tmpfs (12.6MB) — escribir frames raw de 3MB agota la RAM → OOM killer
- VB pool con 4 bloques de 3MB = 12MB — consume casi toda la RAM disponible
- **NUNCA** escribir frames a `/tmp` — usar `/root/` (flash) o streaming por red
- `/root/` está en overlay JFFS2 pero solo tiene ~128KB libres — tampoco sirve para frames
- Única opción viable: streaming por TCP sin guardar a disco

---

## Estado del sensor (I2C readback verificado)

Los valores fueron verificados con readback I2C TANTO desde el driver (debug prints en cmos_isp_init)
COMO desde herramienta externa (i2c_direct) mientras majestic corre. TODOS los registros coinciden.

| Registro | Valor driver | Descripción |
|----------|-------------|-------------|
| 0x30DC | 0x32 | Chip ID IMX662 |
| 0x3014 | 0x03 | INCK_SEL = 27 MHz external (Hi3516 MCLK) |
| 0x3015 | 0x05 | DATARATE_SEL = 891 Mbps/lane (SDR) |
| 0x3018 | 0x04 | WINMODE = HD1080 crop |
| 0x301B | 0x00 | ADDMODE = Non-binning |
| 0x3022 | 0x01 | ADBIT = 12-bit mode |
| 0x3023 | 0x01 | MDBIT = 12-bit |
| 0x302C | 0xBC | HMAX low byte |
| 0x302D | 0x07 | HMAX high byte → HMAX = 1980 |
| 0x3028 | 0xE2 | VMAX low byte |
| 0x3029 | 0x04 | VMAX mid byte |
| 0x302A | 0x00 | VMAX high nibble → VMAX = 1250 |
| 0x3040 | 0x00 | LANEMODE = 1 lane |
| 0x3444 | 0xAC | PLL config |
| 0x3A50 | 0xFF | 12-bit normal output |
| 0x3A51 | 0x03 | 12-bit normal output |
| 0x3A52 | 0x00 | 12-bit normal output |
| 0x3000 | 0x00 | Streaming (exit standby) |

**Power-on defaults** (leídos después de reset del sensor, antes de driver init):
0x3014=0x00, 0x3015=0x02, 0x3018=0x00, 0x3040=0x03 (4-lane!), 0x302C/2D=990, 0x3444=0xFF

**NOTA sobre previos reads con defaults:** Las sesiones anteriores mostraban power-on defaults
porque la herramienta i2c_mclk hacía reset del sensor al habilitar MCLK. Con el driver
correcto, TODOS los registros se escriben correctamente y persisten mientras el ISP corre.

### IMPORTANTE: tabla INCK_SEL/DATARATE_SEL verificada por sweep (2026-08-08)

El sweep relockea el PLL del sensor y mide fps reales en runtime. Resultado:

| INCK_SEL (0x3014) | DATARATE_SEL (0x3015) | FPS real |
|--------------------|----------------------|----------|
| 0x01 (37.125MHz)   | 0x02 (1782Mbps)      | 22.3 |
| 0x01 (37.125MHz)   | 0x05 (891Mbps)       | 22.3 |
| 0x02               | 0x05                 | 11.7 |
| **0x03 (27MHz)**   | **0x05 (891Mbps)**   | **30.7 ✓** |
| 0x04 (24MHz)       | 0x05                 | 34.7 |
| 0x00 (74.25MHz)    | 0x05                 | 9.7 |

**LA COMBINACIÓN CORRECTA es INCK_SEL=0x03 + DATARATE_SEL=0x05 = 30fps exacto.**

El MCLK del SoC es 27MHz (NO 37.125MHz). Con INCK_SEL=0x01 (37.125) el PLL escala
0.727x → pixel clock 54MHz → 21.8fps. La relación 54/74.25 = 8/11 = 27/37.125.

**Síntoma clásico documentado en `pauliustumas/imx662` (GitHub):** frame rate bajo
(~5fps), frames parciales e inestables por INCK_SEL incorrecto respecto al clock real.

**vi_raw_capture.c:** la tabla `imx662_init_common`/`imx662_init_mode` usa INCK=0x01/
DR=0x02 (¡WRONG, 22fps!). Ahora hay override con defaults INCK=0x03/DR=0x05 y flags
`--incksel=N`/`--datarate=N`/`--sweep` para testear. La tabla del driver `libsns_imx662.so`
(majestic) YA usa INCK=0x03/DR=0x05 correcto.

### IMPORTANTE: vc_num — CONTRADICCIÓN RESUELTA (2026-08-09)

**El 2026-08-08 se creyó que vc_num=1 era requerido (IMX662 "envía por VC1").**
**El 2026-08-09 se probó que astro_streamer entrega 30fps EXACTOS con vc_num=0.**
El factor determinante NO es el vc_num sino el **tipo de pool VB**:

- `vi_raw_capture.c` (default `g_vc_num=1`, `create_pool()` usuario con 4 bloques) →
  falla `get_chn_frame 0xa0108016` (aunque antes del rebuild daba 30fps con vc_num=1)
- `astro_streamer.c` (default `g_vc_num=0`, **common pool** vía `get_common_pool_id()` con
  6 bloques, sin `create_pool()`) → **30.0fps estables, send_cnt/vb_fail OK**
- Logs astro_streamer: `vb_exit: 0xa001800d`, `set_cfg: 0xa0018022` (ignorados),
  `get_common_pool_id: 0x0 cnt=1 id[0]=0`, `init_mod_common_pool(VI): 0xa0018018` (OK)
- `vi_raw_capture --vc=0` NO probado — la hipótesis es que `create_pool()` (pool usuario)
  es lo que rompe la entrega, no el vc_num. Test pendiente.

**SECUENCIA QUE FUNCIONA (astro_streamer):**
1. `ot_mpi_vb_exit()` (ret puede fallar, ignorar)
2. `vb_set_cfg(max_pool_cnt=1, common_pool[0]: blk=6)` + `vb_init()`
3. `ot_mpi_vb_get_common_pool_id()` → usar el common pool existente (id[0])
4. `ot_mpi_vb_init_mod_common_pool(OT_VB_UID_VI)`
5. `pipe_attr.vc_num = 0` (NO 1)
6. TCP server en **puerto 5999** + header de 48 bytes ("AS") + `recv_astro.py`

**Sesión que funcionó (2026-08-09):** `astro_streamer 5999` → 30fps bench exacto,
frames 1920x1080 raw12 confirmados por TCP (3.6fps reales = límite 100Mbps, no del sensor).

---

## Pipeline de video

```
IMX662 → MIPI (1 lane, raw12) → MIPI RX (/dev/ot_mipi_rx)
  → VI dev 0 → VI pipe 0 (ISP bypass) → VI chn 0
  → TCP server (puerto 5000) → PC (recv_raw.py + OpenCV)
```

**Frame format:** 1920x1080, raw12 packed (3 bytes/2 pixels), stride=2880, 3,110,400 bytes/frame

---

## Majestic - Estado actual

### Qué funciona
- El .so carga correctamente (sin dlopen errors)
- El sensor escribe TODOS los registros correctamente (verificado con I2C readback)
- El ISP 3A callbacks se ejecutan (cmos_inttime_update, cmos_gains_update, etc.)
- Majestic queda vivo y responde (no hace crash)

### Qué NO funciona
- **VENC timeout** — el encoder H264 no recibe frames (106+ timeouts)
- **Exposure stuck** en 125ms (8fps) — el ISP nunca converge
- **`cmos_slow_framerate_set`** se llama repetidamente — ISP detecta frame rate incorrecto
- **OOM kill** — 25MB RAM total, VB pool consume ~12MB, majestic + kernel ≈ 14MB

### Análisis de VENC timeout — REVISADO (2026-08-08)
El VI dev attr muestra `pixel_rate=68416666` pero el correcto es `74250000` (891Mbps/12bit).
Majestic calcula `lane_rate=821` Mbps, pero el sensor produce 891 Mbps (DATARATE_SEL=0x05).

**CONCLUSIÓN EXPERIMENTAL: el pixel_rate NO es la causa raíz del VENC timeout.**
Se forzó `pixel_rate=74250000` con LD_PRELOAD (ver `fix_pixel_rate.c`/`fix_pixel_rate.so`)
y el VENC timeout PERSISTIÓ. El ISP 3A funciona pero:
- **Exposure stuck en 125ms (8fps)** — ISP mide frame rate ~8fps, no 30fps
- **`cmos_slow_framerate_set` llamado repetidamente** — ISP detecta que el sensor entrega frames muy lentos
- **VB pool de majestic consume demasiada RAM** → OOM mata majestic + dropbear + syslogd

### Diagnóstico real
- `cmos_slow_framerate_set` + exposure 125ms ⇒ el ISP ve el sensor a ~8fps, NO 30fps
- La config MIPI del sensor parece OK (vi_raw_capture funciona y entrega frames 1920x1080)
- El problema es que **majestic (VI_ONLINE_VPSS_ONLINE, ISP procesando) NO recibe frames a 30fps**
- Pista clave en dmesg: `mipirx not set lane mode` (2x en boot) — el MIPI RX no se configuró
- vi_raw_capture usa `isp_bypass=TD_TRUE` y funciona; majestic usa ISP online mode y no

### LD_PRELOAD fix_pixel_rate (probado OK)
- `fix_pixel_rate.so` intercepta `ss_mpi_vi_set_pipe_online_clock` (único símbolo importado por majestic)
- Fuerza pixel_rate 68416666 → 74250000 en runtime
- **PERO no resuelve el VENC timeout** (confirmado experimentalmente)
- Requiere boot limpio (kill de majestic corrompe ISP → ERR_ISP_NOT_SUPPORT)
- Majestic corre manualmente: `LD_PRELOAD=/root/fix_pixel_rate.so majestic` (NO hay S95majestic en /etc/init.d/)

### Divinus — INCOMPATIBLE confirmado en el device
- `/usr/bin/divinus` existe pero imprime: `[hal] Unsupported chip family! Quitting...`
- Solo soporta: Hi3516AV200, Hi3516AV300, Hi3516DV300, Hi3516CV500, Hi3519V101, Hi3556V100, Hi3559V100
- Usa `HI_MPI_*` (SDK comercial), NO `ss_mpi_*`/`ot_mpi_*` como nuestro kernel
- Divinus usa config binario (`.bin`), no `.ini`
- **Descartado definitivamente** — no compatible con Hi3516CV610

### Majestic — closed source
- Binario de S3: `https://openipc.s3-eu-west-1.amazonaws.com/majestic.<family>.<variant>.master.tar.bz2`
- Licencia PROPIETARIA (Prosperity Public License 3.0.0) — NO hay fuente C
- `MAJESTIC_SITE`/`MAJESTIC_SOURCE` en `general/package/majestic/majestic.mk`
- La fuente solo existe en GitHub para integration (smolrtsp) — no hay src del core

### INI parameters PROBADOS (no funcionan)
- `data_rate=1` en `[mipi]` → **ignorado** (majestic sigue mostrando `data_rate=0`)
- `FullLinesStd=1250` en `[vi_dev]` → **no cambia** pixel_rate (sigue 68416666)
- `pixel_rate`, `lane_rate` → **no son campos soportados** del INI
- `lane_rate=821` viene del cálculo interno de majestic (821×1000000/12 = 68416666)

### OOM
- MemTotal: 25748 kB (~25MB usable)
- VB pool: 3110400 × 4 buffers = ~12.4 MB
- Kernel + 36 modules: ~10-12 MB
- Majestic RSS: ~1.6 MB
- Resultado: OOM killer mata majestic Y dropbear Y syslogd

### Soluciones potenciales (actualizado)
1. ~~Agregar pixel_rate al INI~~ → NO es campo soportado
2. ~~LD_PRELOAD fix_pixel_rate~~ → probado, NO resuelve VENC timeout
3. **Reducir VB pool** (de 3+1 a 2+1 buffers) — requiere modificar majestic binary o config
4. **Investigar por qué majestic no recibe frames a 30fps** — el ISP detecta 8fps
5. **Escribir streamer propio con ss_mpi_*** — bypass majestic, control total (ISP bypass funciona via vi_raw_capture)
6. Investigar `mipirx not set lane mode` — MIPI RX no configurado correctamente por majestic

---

## Fixes persistentes aplicados

### 1. S70vendor - sensor type
`/etc/init.d/S70vendor` ahora llama `load_"$vendor" -i -s imx662` (antes usaba default `SNS_TYPE0=sc4336p`).
- `fw_setenv sensor imx662` aplicado en U-Boot env

### 2. load_hisilicon default
`general/package/hisilicon-osdrv-hi3516cv6xx/files/script/load_hisilicon`:
- `SNS_TYPE0=imx662;` (antes era `sc4336p`)

### 3. Driver sensor (múltiples fixes aplicados)
- **INCK_SEL**: 0x01 (interno) → 0x03 (27MHz externo del SoC) — SigmaStar reference lo confirma
- **DATARATE_SEL**: 1782 → 891 Mbps (SDR, `data_rate=0` en INI)
- **0x3A50/51/52**: 0x62/0x01/0x19 → 0xFF/0x03/0x00 (12-bit normal output, SigmaStar)
- **WINMODE**: 0x00 (all pixel) → 0x04 (HD1080 crop, SigmaStar reference)
- **Crop registers**: Agregados 0x303C=0x08, 0x303E=0x80, 0x303F=0x07, 0x3044=0x08, 0x3046=0x38, 0x3047=0x04
- **cmos_restart()**: Cambiado hardcode DATARATE_1782 → DATARATE_891
- **imx662_restart_mode()**: Re-escritura completa de modo regs tras standby enter (WINMODE, crop, ADBIT, MDBIT, 0x3A50-52, DATARATE_SEL, LANEMODE, HMAX, VMAX)
- **cmos_isp_init()**: Agregado sensor reset sequence en `imx662_enable_mclk()`
- **Link fix**: `sensor_common.o` debe linkearse (proporciona cis_register_callback, etc.)

### 4. HMAX byte order
- Antes: HIGH byte primero en 0x302C → HMAX=48135 (WRONG)
- Después: LOW byte primero en 0x302C → HMAX=1980 (CORRECT)

### 5. Timezone
- `/etc/TZ` = `ART3`, `fw_setenv tz ART3` (America/Argentina/Buenos_Aires, UTC-3)

---

## Claves técnicas críticas

### MCLK + Sensor Reset para I2C
El sensor IMX662 necesita **tres cosas** para responder a I2C:
1. **MCLK** habilitado via `OT_MIPI_ENABLE_SENSOR_CLOCK`
2. **Sensor reset** via `OT_MIPI_RESET_SENSOR` → delay 10ms → `OT_MIPI_UNRESET_SENSOR` → delay 100ms
3. Sin estas, el chip está "muerto" y hace NACK → `bsp-i2c: wait idle abort!, RIS: 0x611`

**Secuencia correcta:**
```c
fd = open("/dev/ot_mipi_rx", O_RDWR);
ioctl(fd, OT_MIPI_SET_HS_MODE, &lane_mode);      // necesario
ioctl(fd, OT_MIPI_ENABLE_SENSOR_CLOCK, &clk);     // CRÍTICO
ioctl(fd, OT_MIPI_RESET_SENSOR, &rst);             // CRÍTICO
usleep(10000);
ioctl(fd, OT_MIPI_UNRESET_SENSOR, &rst);           // CRÍTICO
usleep(100000);
close(fd);
// Ahora sí se puede usar /dev/i2c-0
```

**Sin reset:** I2C funciona parcialmente (chip ID lee OK) pero los registros de modo no se aplican correctamente.

### MIPI RX ioctl numbers

| IOCTL | Valor | Descripción |
|-------|-------|-------------|
| OT_MIPI_SET_HS_MODE | `_IOW('m', 0x0b, int)` | Configurar modo high-speed |
| OT_MIPI_ENABLE_SENSOR_CLOCK | `_IOW('m', 0x10, int)` | Habilitar MCLK al sensor |
| OT_MIPI_RESET_SENSOR | `_IOW('m', 0x05, int)` | Poner sensor en reset |
| OT_MIPI_UNRESET_SENSOR | `_IOW('m', 0x06, int)` | Sacar sensor de reset |
| OT_MIPI_SET_DEV_ATTR | magic `'m'` | Configurar dev (input_mode, lanes, etc.) |
| OT_MIPI_UNRESET_MIPI | magic `'m'` | Sacar MIPI de reset |

### Init sequence del sensor (3 fases)
El driver Linux V4L2 escribe registros en este orden:
1. **Common regs** (149 regs): INCK_SEL, PLL, MIPI TX (`0x44xx`), PHY, timing — todo ANTES del standby exit
2. **Standby exit** + 100ms PLL lock wait
3. **Mode regs** (14 regs): ADDMODE, ADBIT, HMAX, VMAX — DESPUÉS del standby exit

El registro 0x3000=0x01 entra en standby. 0x3000=0x00 sale de standby. 0x3001=0x00 inicia streaming.

### VB (Video Buffer) management
- **Problema:** `ot_mpi_vb_exit()` destruye el estado VB del kernel (los módulos se cargan con `max_pool_cnt=0`)
- **Solución:** Llamar `vb_exit()` → `vb_set_cfg(max_pool_cnt=1)` → `vb_init()` → `vb_create_pool()`
- **Sin vb_exit():** `vb_create_pool()` falla porque el kernel tiene `max_pool_cnt=0`
- **Con vb_exit():** funciona pero el estado del kernel se corrompe — `sys_exit()` causa kernel panic
- **Cleanup correcto:** VI pipeline teardown → `_exit(0)` (NO llamar `sys_exit()` ni `vb_destroy_pool()`)
- `vb_set_cfg()` retorna 0x0 (success) sin vb_exit() pero no actualiza el `max_pool_cnt` real del kernel

### I2C controller (bsp-i2c)
- Driver custom HiSilicon, NO DesignWare
- Source: `output/build/linux-custom/drivers/i2c/busses/i2c-bsp.c`
- Registros: `BSP_I2C_GLB=0x00`, `BSP_I2C_TXF=0x20`, `BSP_I2C_RXF=0x24`, `BSP_I2C_CMD_BASE=0x30`, `BSP_I2C_STAT=0xd8`, `BSP_I2C_INTR_RAW=0xe0`
- `INTR_ABORT_MASK = BIT(0) | BIT(11) = 0x801`
- `bsp_i2c_clr_irq()` escribe W1C a INTR_RAW — limpia abort antes de cada transferencia
- El módulo `open_sensor_i2c` crea clientes dummy en addr 0x36 (0x6c>>1) en los 3 adaptadores

### OOM (Out of Memory)
- **Causa:** Escribir frames raw a `/tmp` (tmpfs) consume RAM
- **Umbral:** 2 frames de 3MB = 6MB en tmpfs + VB pool 12MB + kernel ≈ 32MB → OOM
- **Solución:** TCP streaming sin disco, o write a flash (poco espacio disponible)
- **Error OOM:** `vi_raw_capture invoked oom-killer` → `Killed process 1100 (vi_raw_capture) total-vm:3964kB`

---

## Archivos del proyecto

Todos los utilitarios de prueba viven en `utils/` (compilar/ejecutar desde ahí).

### `utils/astro_streamer.c` — Streaming server TCP (EL QUE FUNCIONA)
- Compilación: ver abajo
- Inicializa todo desde cero (MPI, VB, MCLK, sensor I2C, VI pipeline)
- TCP server en puerto 5000 default (usar **5999**: `astro_streamer 5999`)
- Envía frames raw12 por TCP con **header de 48 bytes** (magic "AS") — ver `recv_astro.py`
- **Secuencia VB que funciona:** `vb_exit()` → `vb_set_cfg(common_pool blk=6)` → `vb_init()`
  → `vb_get_common_pool_id()` (usar common pool, NO `create_pool()`) → `init_mod_common_pool(VI)`
- `pipe_attr.vc_num = 0` (default) → **30fps exactos confirmados**
- Flags: `--bench` / `--bench=N` (mide fps sin TCP, N=segundos), `--sweep`, `--incksel=N`,
  `--datarate=N`, `--vc=N` (override vc_num), `--shrconv=N`
- No escribe a disco (evita OOM). Cleanup: `_exit(0)` para evitar kernel panic
- **IMPORTANTE:** `kill -9` corrompe el estado VB → siguiente run falla "VB pool FAILED"
  → requiere reboot limpio. Terminar con SIGTERM (romper el `accept()` con un cliente TCP
  temporal si no hay cliente conectado) o reboot.

### `utils/vi_raw_capture.c` — Streaming server TCP (ROTO tras rebuild)
- Compilación: ver abajo
- Inicializa todo desde cero (MPI, VB, MCLK, sensor I2C, VI pipeline)
- TCP server en puerto 5000 (configurable por arg)
- Envía frames raw12 por TCP con header de 12 bytes
- No escribe a disco (evita OOM)
- Cleanup: `_exit(0)` para evitar kernel panic
- Flags: `--bench` (mide fps sin TCP), `--sweep` (barre INCK_SEL/DATARATE relockeando
  PLL), `--incksel=N` (override 0x3014), `--datarate=N` (override 0x3015), `--vc=N`
  (override `pipe_attr.vc_num`, default 1)
- **IMPORTANTE:** defaults corregidos INCK_SEL=0x03 + DATARATE_SEL=0x05 (30fps). La
  tabla estática `imx662_init_common`/`imx662_init_mode` aún tiene INCK=0x01/DR=0x02
  (WRONG) pero el override post-standby-exit la corrige en runtime.
- **ROTO desde el rebuild (2026-08-09):** usa `create_pool()` (pool usuario, 4 bloques) +
  vc_num=1 → `get_chn_frame 0xa0108016`. NO usar — ver astro_streamer arriba.
- **IMPORTANTE:** `kill -9` corrompe el estado VB → siguiente run falla "VB pool FAILED"
  → requiere reboot limpio. Terminar con SIGTERM (teardown limpio) o reboot.

### `utils/recv_astro.py` — Receiver OpenCV (PC, para astro_streamer)
- Recibe frames por TCP del SoC en puerto **5999**
- Lee header de 48 bytes (magic "AS") → convierte raw12 → visualización (Bayer/grayscale)
- Controles: q=salir, s=screenshot, etc.

### `utils/recv_raw.py` — Receiver OpenCV (PC)
- Recibe frames por TCP del SoC
- Convierte raw12 → visualización (Bayer color o grayscale)
- Controles: q=salir, s=screenshot, g=grayscale, b=bayer, +/- brillo

### `utils/i2c_recovery.c` — GPIO bit-bang I2C bus recovery
### `utils/i2c_test.c` — I2C + SPI diagnostic test
### `utils/i2c_direct.c` / `i2c_dump.c` / `i2c_read.c` / `i2c_mclk.c` — I2C diagnostics

### `utils/fix_pixel_rate.c` / `utils/fix_pixel_rate.so` — LD_PRELOAD pixel_rate fix
- Intercepta `ss_mpi_vi_set_pipe_online_clock(pipe, pixel_rate)`
- Fuerza pixel_rate 68416666 → 74250000 en runtime
- Compilación: `$CC -fPIC -shared -o fix_pixel_rate.so fix_pixel_rate.c -ldl`
- Deploy: `/root/fix_pixel_rate.so` (en el device)
- Uso: `LD_PRELOAD=/root/fix_pixel_rate.so majestic`
- **No resuelve VENC timeout** (probado 2026-08-08)

### Driver sensor (build output, se sobreescribe al rebuild)
```
output/build/hisilicon-opensdk-ff20187b/libraries/sensor/hi3516cv6xx/sony_imx662/
```
- `imx662_cfg.h` — Init sequence completa (149 common + 14 mode regs)
- `imx662_sensor_ctrl.c` — HMAX byte order fix, linear_init
- `imx662_cmos.h` — Definiciones (DATARATE_1782, HMAX, VMAX, etc.)
- `imx662_cmos.c` — cmos_isp_init, cmos_restart, enable_mclk

### Archivos modificados en build tree
- `general/overlay/etc/init.d/S70vendor` — fix: `load_"$vendor" -i -s imx662`
- `general/package/hisilicon-osdrv-hi3516cv6xx/files/script/load_hisilicon` — `SNS_TYPE0=imx662;`

### Headers kernel
- SDK: `output/host/opt/ext-toolchain/sdk/`
- Kernel: `output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx/`
- MIPI RX: `output/build/hisilicon-opensdk-ff20187b/kernel/mipi_rx/hi3516cv6xx/include/`
- ISP ext: `output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx/isp_ext_inc/`

---

## Compilación

Desde `utils/` (paths relativos a la raíz del firmware):

```bash
SDK=../output/host/opt/ext-toolchain/sdk
CC=../output/host/bin/arm-openipc-linux-musleabi-gcc
KO=../output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx
MIPI=../output/build/hisilicon-opensdk-ff20187b/kernel/mipi_rx/hi3516cv6xx/include

$CC -o vi_raw_capture vi_raw_capture.c \
  -I$SDK/include -I$KO -I$KO/isp_ext_inc -I$MIPI \
  -L$SDK/lib -Wl,-rpath,$SDK/lib \
  -lss_mpi -lss_mpi_sysmem -lss_mpi_sysbind -lot_osal \
  -lsecurec -lpthread -ldl -lm
```

---

## Subir al device (SSH paramiko)

```python
import paramiko, base64, time

ssh = paramiko.SSHClient()
ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
ssh.connect('192.168.1.5', username='root', password='12345', timeout=10)
transport = ssh.get_transport()

with open('vi_raw_capture', 'rb') as f:
    data = f.read()
encoded = base64.b64encode(data).decode()
chunks = [encoded[i:i+4000] for i in range(0, len(encoded), 4000)]

ch = transport.open_session()
ch.exec_command('rm -f /tmp/vi_raw_capture /tmp/lib.b64')
ch.recv_exit_status()

for i, chunk in enumerate(chunks):
    cmd = f'echo -n "{chunk}" > /tmp/lib.b64' if i == 0 else f'echo -n "{chunk}" >> /tmp/lib.b64'
    ch = transport.open_session()
    ch.exec_command(cmd)
    ch.recv_exit_status()

ch = transport.open_session()
ch.exec_command('base64 -d /tmp/lib.b64 > /tmp/vi_raw_capture && chmod +x /tmp/vi_raw_capture && rm /tmp/lib.b64')
ch.recv_exit_status()
```

---

## Secuencia de uso

### Streaming raw12 (modo actual) — astro_streamer

**En el device** (después de reboot limpio):
```bash
/tmp/astro_streamer 5999     # EL QUE FUNCIONA (30fps, header 48B "AS")
/tmp/astro_streamer 5999 --bench 10   # medir fps sin TCP
```

**En el PC:**
```bash
pip3 install opencv-python numpy
python3 recv_astro.py 192.168.1.16 5999
```

### NOTA: vi_raw_capture NO funciona tras el rebuild (2026-08-09)
```bash
/tmp/vi_raw_capture 5000     # get_chn_frame 0xa0108016 — ROTO
```
Usar `astro_streamer` (common pool + vc_num=0) en su lugar.

### Si majestic está corriendo (test I2C rápido)
```bash
killall -9 majestic
/tmp/astro_streamer 5999
```

---

## Driver Linux V4L2 de referencia

- https://github.com/AraKiLiu/imx662-v4l2-driver/blob/main/imx662.c
- `mode_common_regs[]`: INCK_SEL, PLL, MIPI TX/PHY, timing
- `mode_2k_regs[]`: 1920x1100 all-pixel, HMAX=990, VMAX=1250
- `PIXEL_RATE = 74,250,000 Hz`
- Link freqs: 0x02=1782Mbps, 0x05=891Mbps, etc.

## Referencias GitHub — conclusión común (2026-08-09)

Se revisaron 8 repos de IMX662 para validar la configuración del sensor:

| Repo | Plataforma | Aporte clave |
|------|-----------|--------------|
| `pauliustumas/imx662` | RPi4, 74.25MHz cristal | **INCK_SEL correcto = TODO.** Con 24MHz (INCK 0x04): frames parciales (1–275/1100), ~5fps, inestable. Con 74.25MHz (INCK 0x00): 36–60fps estable. Síntoma clásico de reloj mal configurado. |
| `libc0607/imx662_modes` | OpenIPC + SigmaStar (SSC338Q) | **27MHz input + 2 lanes funciona.** Tabla modos: 1920x1080@30fps 12bit → VMAX=1250, HMAX=1980 (¡exacto a nuestro readback!). INCK=27MHz + DATARATE=891Mbps = 30fps. |
| `BellssGit/IMX662_module_for_raspberry_pi` | Hardware RPi | Datasheet, SRM, register map. I2C 0x1a (0x34). Referencia hardware. |
| `will127534/imx662-v4l2-driver` | RPi5/RP1 | Tabla link-frequency: 891Mbps=30fps@1080p, 1782Mbps=60fps. |
| `AraKiLiu/imx662-v4l2-driver` | RPi5 | 12-bit + binning fix. Requiere kernel 6.12+. |
| `dio4/imx662-camera-project` | RPi4 raw | 10-bit raw directo, "green frames" resuelto con demosaic RG→BGR. |
| `neskin/imx662-camera-dashboard` | RPi | Solo dashboard web, no aporta. |
| `pauliustumas` nota | — | "the ISP is what makes it look good" — el raw demosaicado se ve mal, el ISP hardware es lo que da imagen buena. |

**CONCLUSIÓN COMÚN:**
1. **INCK_SEL debe coincidir con el clock real del sensor.** 2 fuentes independientes confirman: reloj mal configurado → ~5fps + frames parciales. Nuestra config INCK=0x03 (27MHz) + DR=0x05 (891Mbps) es CORRECTA y validada.
2. **Nuestra tabla de modos es idéntica a la de libc0607** (1920x1080@30, 12bit, VMAX=1250, HMAX=1980) que funciona en OpenIPC.
3. **Modo 12-bit debe seleccionarse explícitamente** (default 10-bit da cero/garbage pixeles en varias plataformas).
4. **El problema de majestic NO es config del sensor** — es del pipeline VI/ISP online (pixel_rate, `mipirx not set lane mode`, ISP online vs bypass). La config del sensor está validada contra referencias que funcionan.
5. **Dirección I2C 0x1a y chip ID 0x30DC=0x32** son consistentes en todos los repos.

**DECISIÓN:** seguir con streamer propio (ISP bypass, vi_raw_capture/astro_streamer), no seguir peleando con majestic.

---

## Módulos del kernel (36)

Cargados por `load_hisilicon -i -s imx662`:
`open_sys_config`, `open_osal`, `open_mmz`, `open_base`, `open_vb`, `open_vca`, `open_sys`, `open_rgn`, `open_vpp`, `open_vgs`, `open_vpss`, `open_vi`, `open_isp`, `open_chnl`, `open_rc`, `open_venc`, `open_h264e`, `open_h265e`, `open_jpege`, `open_svp_npu`, `open_ive`, `open_pwm`, `open_piris`, `open_sensor_i2c`, `open_sensor_spi`, `open_aio`, `open_ai`, `open_ao`, `open_aenc`, `open_adec`, `open_acodec`, `open_mipi_rx`, `open_pm`, `open_wdt`, `open_cipher`, `open_km`

---

## Configuración ini sensor

```ini
[sensor]
sensor_type=imx662
mode=WDR_MODE_NONE
dllfile=libsns_imx662.so

[mode]
input_mode=INPUT_MODE_MIPI
raw_bitness=12

[mipi]
lane_id=0|-1|-1|-1

[isp_image]
isp_framerate=30
isp_bayer=BAYER_RGGB

[vi_dev]
input_mod=VI_MODE_MIPI
work_mod=VI_WORK_MODE_1Multiplex
mask_0=0xFFF0000
scan_mode=VI_SCAN_PROGRESSIVE
devrect_w=1920
devrect_h=1080
```

---

## Pendiente

- [ ] **BUG ACTIVO (2026-08-10): frame blanco estático `f0 0f ff` tras reboot** → MIPI RX no recibe datos. Ver `CONTEXT.md` + `tools/prompts/diagnostico-mipi.md`
- [ ] Verificar imagen real con `recv_astro.py` + OpenCV
- [ ] Evaluar calidad de imagen (Bayer pattern correcto, exposición, etc.)
- [ ] Test pendiente: `vi_raw_capture --vc=0` para confirmar si lo que rompe es `create_pool()` (pool usuario) o el vc_num=1
- [ ] Investigar CRC errors en MIPI RX (`vc0_crc_err: 1935`)
- [ ] Considerar si el kernel panic de `sys_exit()` se puede resolver sin vb_exit()
- [ ] Investigar `mipirx not set lane mode` en dmesg (MIPI RX no configurado)
- [x] ~~Majestic vc_num=1 test~~ → **DECIDIDO (2026-08-09): abandonar majestic**, streamer propio con ISP bypass
- [x] ~~Decidir streamer propio vs majestic~~ → **astro_streamer (vc_num=0 + common pool) es EL que funciona**
