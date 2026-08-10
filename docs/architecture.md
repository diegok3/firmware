# Architecture — IMX662 + Hi3516CV610 video pipeline

## Pipeline de video

```
IMX662 sensor (Sony, 1 lane MIPI, raw12)
  │  MIPI CSI-2 891 Mbps (DATARATE_SEL=0x05)
  ▼
MIPI RX  (/dev/ot_mipi_rx, kernel module open_mipi_rx)
  │  PHY configurada por ioctl (lane_id[0]=0, DATA_TYPE_RAW_12BIT)
  ▼
VI dev 0 → VI pipe 0 (ISP bypass) → VI chn 0
  │  vc_num=0, common pool VB (6 bloques, via get_common_pool_id)
  ▼
get_chn_frame → mmap phys addr → TCP 5999 (header 48B "AS" + raw12)
  ▼
PC: recv_astro.py (LSB-first decode) → OpenCV → PNG/visualización
```

## Frame format

| Campo | Valor |
|-------|-------|
| Resolución | 1920×1080 |
| Bit depth | raw12 packed (3 bytes / 2 píxeles) |
| Stride | 2880 bytes |
| Tamaño frame | 3,110,400 bytes |
| Pixel clock | 74.25 MHz |
| Line time | 26.6667 µs (1980/74.25MHz) |
| Frame rate | 30 fps (VMAX=1250, HMAX=1980) |

## Raw12 payload — LSB-first (CRÍTICO, decisión 0003)

Triplet `[b0, b1, b2]` por 2 píxeles:
- `b0` siempre múltiplo de 16 (nibble bajo = 0)
- `b1` siempre < 16 (nibble alto = 0)
- `b2` rango completo

⇒ El sensor entrega datos 8-bit (`v<<4`) en contenedor 12-bit **LSB-first**.

**Decode correcto:**
```
v0 = ((b1 & 0xF) << 4) | (b0 >> 4)
v1 = b2
```

Implementado en `utils/recv_astro.py:raw12_to_uint16` y `utils/recv_raw.py:unpack_raw12`.

**⚠️ Patrón `f0 0f ff`** = frame blanco (BUG ACTIVO, ver CONTEXT.md). Es la firma del VI
cuando el MIPI RX no recibe datos reales.

## Sensor IMX662 config (validada contra 8 repos GitHub)

| Registro | Valor | Nota |
|----------|-------|------|
| 0x3014 (INCK_SEL) | 0x03 | 27 MHz externo (MCLK del SoC) |
| 0x3015 (DATARATE_SEL) | 0x05 | 891 Mbps/lane SDR |
| 0x3018 (WINMODE) | 0x04 | HD1080 crop |
| 0x301B (ADDMODE) | 0x00 | Non-binning |
| 0x3022/23 (ADBIT/MDBIT) | 0x01 | 12-bit |
| 0x302C/2D (HMAX) | 0xBC/0x07 | 1980 (LOW byte first!) |
| 0x3028/29/2A (VMAX) | 0xE2/0x04/0x00 | 1250 |
| 0x3040 (LANEMODE) | 0x00 | 1 lane |
| 0x3A50/51/52 | 0xFF/0x03/0x00 | 12-bit normal output |
| 0x30DC | 0x32 | Chip ID |

**Clave:** HMAX se escribe LOW byte primero (0x302C=0xBC, 0x302D=0x07). Si se escribe al
revés, HMAX=48135 (corrupto).

## Secuencia de init del streamer (astro_streamer)

1. `vb_exit()` (ret puede fallar, ignorar)
2. `vb_set_cfg(max_pool_cnt=1, common_pool[0]: blk=6)` + `vb_init()`
3. `vb_get_common_pool_id()` → usar common pool existente (id[0]), **NO create_pool()**
4. `init_mod_common_pool(OT_VB_UID_VI)`
5. `pipe_attr.vc_num = 0` (NO 1)
6. `enable_mclk_and_reset_sensor()`: HS_MODE → ENABLE_SENSOR_CLOCK → RESET → 10ms → UNRESET → 500ms
7. MIPI RX: `SET_DEV_ATTR` (1 lane, RAW_12BIT) + `UNRESET_MIPI`
8. Sensor I2C: 149 common regs + 14 mode regs (en standby) + override INCK/DR + exit standby + PLL lock
9. VI pipeline: set_dev_attr → enable_dev → bind → create_pipe → vc_num → start_pipe → enable_chn
10. TCP server 5999

**⚠️ Posible fix pendiente (bug 2026-08-10):** el streamer NO llama `OT_MIPI_ENABLE_MIPI_CLOCK`
(ioctl 0x0c). El driver de resume usa la secuencia:
`enable_mipi_clock → reset_mipi → set_attr → unreset_mipi`. Investigar.

## Reglas de memoria (OOM killer)

- /tmp es tmpfs (12.6MB) — NUNCA escribir frames raw de 3MB ahí
- VB pool: 6 bloques × 3MB = ~18MB → casi toda la RAM
- Única vía viable: streaming TCP sin disco, o flash (128KB libres)
- Terminar streamer con SIGTERM (nunca kill -9 → corrompe VB → reboot)

## Módulos kernel involucrados

`open_mipi_rx` (MIPI RX + sensor clock/reset), `open_vi`, `open_isp` (ISP bypass), `open_vb`,
`open_sys`, `open_sensor_i2c`. Cargados por `load_hisilicon -i -s imx662`.

## IOCTLs MIPI RX relevantes

| IOCTL | Valor | Uso |
|-------|-------|-----|
| OT_MIPI_SET_DEV_ATTR | _IOW('m',0x01) | Configurar lane/data type |
| OT_MIPI_SET_PHY_CMVMODE | _IOW('m',0x04) | Voltaje común PHY (LVDS, no usado) |
| OT_MIPI_RESET_SENSOR | _IOW('m',0x05) | Reset sensor |
| OT_MIPI_UNRESET_SENSOR | _IOW('m',0x06) | Quitar reset sensor |
| OT_MIPI_RESET_MIPI | _IOW('m',0x07) | Reset MIPI RX |
| OT_MIPI_UNRESET_MIPI | _IOW('m',0x08) | Quitar reset MIPI RX |
| OT_MIPI_SET_HS_MODE | _IOW('m',0x0b) | Lane divide mode |
| OT_MIPI_ENABLE_MIPI_CLOCK | _IOW('m',0x0c) | Habilitar pixel clock RX ⚠️ no usado |
| OT_MIPI_DISABLE_MIPI_CLOCK | _IOW('m',0x0d) | Deshabilitar |
| OT_MIPI_ENABLE_SENSOR_CLOCK | _IOW('m',0x10) | MCLK al sensor |
| OT_MIPI_SET_EXT_DATA_TYPE | _IOW('m',0x12) | Data types custom |
