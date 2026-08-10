# CONTEXT.md — Estado actual del proyecto

> **Documento de estado vivo.** Se actualiza al final de cada sesión.
> Si algo cambió, acá va. Para el historial de qué se rompió, ver `CHANGELOG.md`.

Última actualización: **2026-08-10 (sesión: bug blanco RESUELTO = saturación sin lente)**

---

## ✅ RESUELTO (2026-08-10): el "frame blanco" era SATURACIÓN, no un bug

**El misterio del frame blanco `f0 0f ff` está resuelto: NO era un bug del MIPI RX.
Era el sensor **saturado a blanco** porque está sin lente y capta la luz de todo
el entorno a exposición larga (exp 33040us + ganancia máxima).**

**Evidencia (2026-08-10, sesión de hoy):**
- **Tapar el sensor** (cubrir la luz) → los frames cambian de inmediato:
  `unique=130` (antes 3), `diff_bytes=261054` entre frames (antes 0) ⇒ **datos reales**.
- El MIPI RX recibe y parsea paquetes reales del sensor: `freq_measure=896MHz`
  (~891Mbps correcto), `mipi_ph_d0: 0x01` (Frame Start), `mipi_vc0_w/h=1920x1080`,
  `vsync_cnt=10`, solo 2 CRC errors.
- El VI entrega frames a 30fps sin errores: `int_cnt=55706, send_cnt=55705,
  frame_rate=30, 0 lost, 0 vb_fail` → el pipeline MIPI→VI **funciona completo**.
- `recv_astro.py` (PC) con botones de ganancia/exposición muestra imagen real.

**Conclusión:** `astro_streamer` (MIPI RX→VI→TCP) está **funcional y VERIFICADO**.
El frame blanco estático aparece solo cuando el sensor sin lente se satura
(mucha luz + exposición larga). Es comportamiento esperado de hardware, no un bug.

**Para "ver algo" con sensor desnudo:** lente M12 (4-6mm) o cámara estenopeica
(pinhole ~0.15mm a ~10-20mm del chip). Un ocular de telescopio solo NO forma imagen
sin el objetivo del telescopio.

**Diagnóstico rápido si aparece blanco de nuevo:** correr
`python3 /tmp/opencode/recv_test.py` → si `unique=3, diff=0` es saturación;
tapar el sensor y verificar que `unique` suba y `diff_bytes > 0`.

---

## ✅ Estado funcional estable (referencia "sabe bien")

| Componente | Estado |
|-----------|--------|
| Sensor IMX662 detectado | ✅ Chip ID 0x32, 1 lane, i2c-0 addr 0x1a (0x34 8-bit) |
| Config sensor | ✅ INCK_SEL=0x03 (27MHz) + DATARATE_SEL=0x05 (891Mbps) = **30fps exactos** |
| `astro_streamer` init | ✅ Secuencia completa: MCLK→sensor I2C→VI pipeline→TCP 5999 |
| Streamer TCP | ✅ Puerto 5999, header 48B magic "AS", frames raw12 3,110,400 B |
| Receive en PC | ✅ `recv_astro.py` (LSB-first decode) |
| Control en PC | ✅ Botones clickables en ventana: AGAIN ±, DGAIN ±, EXP ±, AUTO (comandos A/D/T) |
| golden set correcto | ✅ `golden_sets/imx662-lensless/` (ruido-lluvia = correcto sin lente) |

## Decisiones tomadas (detalle en docs/decisions/)

| Decisión | ADR |
|----------|-----|
| Abandonar majestic, streamer propio con ISP bypass | `docs/decisions/0001-abandonar-majestic.md` |
| `astro_streamer` es EL streamer que funciona (vc_num=0 + common pool) | `docs/decisions/0002-astro-streamer.md` |
| Payload raw12 es **LSB-first** (`v<<4`), decode con `>>4` | `docs/decisions/0003-raw12-lsb-first.md` |
| INCK_SEL=0x03 + DATARATE_SEL=0x05 = 30fps (verificado por sweep) | `docs/decisions/0004-incksel-datarate.md` |

## Hardware

- Sensor Sony IMX662, 1 lane MIPI físico, raw12, i2c-0 addr 0x1a
- SoC Hi3516CV610, ~25MB RAM usable (mem=32M), Ethernet 100Mbps
- Device: `192.168.1.16`, root/12345, SSH paramiko (SFTP roto → upload base64 chunked)
- Flash: boot 320KB + kernel 3MB + rootfs 8MB + appfs 4.2MB + rootfs_data 512KB
- **NUNCA escribir frames a /tmp** (OOM killer). Solo streaming TCP o flash con cuidado.

## Convenciones de sesión

- Terminar streamer con SIGTERM (nunca kill -9 → corrompe VB, requiere reboot)
- Capturar frames SIEMPRE en el PC, nunca en el SoC
- Al terminar sesión: actualizar `CONTEXT.md` (estado) + `CHANGELOG.md` (qué cambió/qué rompió)
- Antes de declarar "funciona": comparar contra `golden_sets/`
