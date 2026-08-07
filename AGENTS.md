# AGENTS.md - Estado del Proyecto IMX662 + Hi3516CV610 (OpenIPC)

## Resumen

**Sensor:** Sony IMX662, 1 lane MIPI, 1080p30 raw12, i2c-0 addr 0x34
**SoC:** Hi3516CV610, 32MB RAM, OpenIPC
**Estado:** Streaming raw12 por TCP funcional, majestic deshabilitado permanentemente

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
| Red | Ethernet 100Mbps, IP 192.168.1.5 |
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

## Estado del sensor (I2C readback)

| Registro | Valor | Descripción |
|----------|-------|-------------|
| 0x30DC | 0x32 | Chip ID IMX662 |
| 0x3014 | 0x01 | INCK_SEL = 37.125 MHz internal oscillator |
| 0x3015 | 0x02 | DATARATE_SEL = 1782 Mbps/lane |
| 0x3018 | 0x00 | WINMODE = All pixel (no binning) |
| 0x301B | 0x00 | ADDMODE = Non-binning |
| 0x3022 | 0x00 | ADBIT = 12-bit normal mode |
| 0x302C | 0xBC | HMAX low byte |
| 0x302D | 0x07 | HMAX high byte → HMAX = 1980 |
| 0x3028 | 0xE2 | VMAX low byte |
| 0x3029 | 0x04 | VMAX mid byte |
| 0x302A | 0x00 | VMAX high nibble → VMAX = 1250 |
| 0x3040 | 0x00 | LANEMODE = 1 lane |
| 0x3444 | 0xAC | PLL config |
| 0x3A50 | 0x62 | Normal 12-bit output |
| 0x3A51 | 0x01 | Normal 12-bit output |
| 0x3A52 | 0x19 | Normal 12-bit output |

---

## Pipeline de video

```
IMX662 → MIPI (1 lane, raw12) → MIPI RX (/dev/ot_mipi_rx)
  → VI dev 0 → VI pipe 0 (ISP bypass) → VI chn 0
  → TCP server (puerto 5000) → PC (recv_raw.py + OpenCV)
```

**Frame format:** 1920x1080, raw12 packed (3 bytes/2 pixels), stride=2880, 3,110,400 bytes/frame

---

## Majestic deshabilitado

- `/etc/init.d/S95majestic` eliminado con `rm -f`
- `output/build/.../S95majestic` eliminado del build tree
- Al hacer reboot, majestic NO arranca automáticamente
- El sensor queda streaming (init I2C + standby exit se hace una vez en boot por `S70vendor`)

---

## Fixes persistentes aplicados

### 1. S70vendor - sensor type
`/etc/init.d/S70vendor` ahora llama `load_"$vendor" -i -s imx662` (antes usaba default `SNS_TYPE0=sc4336p`).
- `fw_setenv sensor imx662` aplicado en U-Boot env

### 2. load_hisilicon default
`general/package/hisilicon-osdrv-hi3516cv6xx/files/script/load_hisilicon`:
- `SNS_TYPE0=imx662;` (antes era `sc4336p`)

### 3. Driver sensor (3 bugs corregidos)
- `DATARATE_SEL`: 891 → 1782 Mbps
- `0x3A50/51/52`: valores incorrectos → 0x62/0x01/0x19
- `INCK_SEL`: 0x03 (27MHz externo) → 0x01 (37.125MHz interno)

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

### `vi_raw_capture.c` — Streaming server TCP
- Compilación: ver abajo
- Inicializa todo desde cero (MPI, VB, MCLK, sensor I2C, VI pipeline)
- TCP server en puerto 5000 (configurable por arg)
- Envía frames raw12 por TCP con header de 12 bytes
- No escribe a disco (evita OOM)
- Cleanup: `_exit(0)` para evitar kernel panic

### `recv_raw.py` — Receiver OpenCV (PC)
- Recibe frames por TCP del SoC
- Convierte raw12 → visualización (Bayer color o grayscale)
- Controles: q=salir, s=screenshot, g=grayscale, b=bayer, +/- brillo

### `i2c_recovery.c` — GPIO bit-bang I2C bus recovery
### `i2c_test.c` — I2C + SPI diagnostic test

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

```bash
SDK=output/host/opt/ext-toolchain/sdk
CC=output/host/bin/arm-openipc-linux-musleabi-gcc
KO=output/build/hisilicon-opensdk-ff20187b/kernel/include/hi3516cv6xx
MIPI=output/build/hisilicon-opensdk-ff20187b/kernel/mipi_rx/hi3516cv6xx/include

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

### Streaming raw12 (modo actual)

**En el device** (después de reboot limpio):
```bash
/tmp/vi_raw_capture          # puerto 5000 default
/tmp/vi_raw_capture 7000     # puerto custom
```

**En el PC:**
```bash
pip3 install opencv-python numpy
python3 recv_raw.py 192.168.1.5        # puerto 5000
python3 recv_raw.py 192.168.1.5 7000   # puerto custom
```

### Si majestic está corriendo (test I2C rápido)
```bash
killall -9 majestic
/tmp/vi_raw_capture
```

---

## Driver Linux V4L2 de referencia

- https://github.com/AraKiLiu/imx662-v4l2-driver/blob/main/imx662.c
- `mode_common_regs[]`: INCK_SEL, PLL, MIPI TX/PHY, timing
- `mode_2k_regs[]`: 1920x1100 all-pixel, HMAX=990, VMAX=1250
- `PIXEL_RATE = 74,250,000 Hz`
- Link freqs: 0x02=1782Mbps, 0x05=891Mbps, etc.

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

- [ ] Verificar imagen real con `recv_raw.py` + OpenCV
- [ ] Evaluar calidad de imagen (Bayer pattern correcto, exposición, etc.)
- [ ] Investigar CRC errors en MIPI RX (`vc0_crc_err: 1935`)
- [ ] Considerar si el kernel panic de `sys_exit()` se puede resolver sin vb_exit()
