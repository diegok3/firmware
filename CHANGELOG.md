# CHANGELOG.md — Qué cambió y qué rompió

> Formato: fecha — qué cambió — qué se rompió — cómo se resolvió (o estado).
> **El propósito de este archivo es aprender de los regresiones.** Siempre anotar
> lo que ROMPIÓ, no solo lo que funcionó.

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
