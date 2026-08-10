# ADR-0004: INCK_SEL=0x03 + DATARATE_SEL=0x05 = 30fps exactos

**Fecha:** 2026-08-08
**Estado:** Aceptado

## Contexto

El sensor daba frame rate bajo (~5-22fps) con la config inicial. Se hizo un sweep en
runtime (relockeando el PLL del sensor) de INCK_SEL (0x3014) y DATARATE_SEL (0x3015):

| INCK_SEL | DATARATE_SEL | FPS real |
|----------|--------------|----------|
| 0x01 (37.125MHz) | 0x02 (1782Mbps) | 22.3 |
| 0x01 (37.125MHz) | 0x05 (891Mbps) | 22.3 |
| 0x02 | 0x05 | 11.7 |
| **0x03 (27MHz)** | **0x05 (891Mbps)** | **30.7 ✓** |
| 0x04 (24MHz) | 0x05 | 34.7 |
| 0x00 (74.25MHz) | 0x05 | 9.7 |

## Decisión

Usar **INCK_SEL=0x03 (27MHz)** + **DATARATE_SEL=0x05 (891Mbps)** = 30fps exacto.

## Justificación

- El MCLK del SoC es **27MHz** (NO 37.125MHz)
- Con INCK_SEL=0x01 el PLL escala 0.727x → pixel clock 54MHz → 21.8fps
- La relación 54/74.25 = 8/11 = 27/37.125 — el INCK debe coincidir con el clock real
- Síntoma clásico documentado en `pauliustumas/imx662`: INCK mal → ~5fps + frames parciales
- Confirmado por `libc0607/imx662_modes`: 27MHz + 891Mbps = 1920x1080@30fps 12bit, VMAX=1250, HMAX=1980 (idéntico a nuestro readback)

## Consecuencias

- La tabla estática del driver (`imx662_init_common`/`imx662_init_mode`) usa INCK=0x01/DR=0x02
  (WRONG). Se aplica override post-standby-exit con los defaults correctos
- Flags `--incksel=N` / `--datarate=N` / `--sweep` en astro_streamer para testear
- La tabla de `libsns_imx662.so` (majestic) ya usa INCK=0x03/DR=0x05 correcto

## Referencias

- `utils/astro_streamer.c` (defaults g_incksel=0x03, g_datarate=0x05)
- https://github.com/pauliustumas/imx662
- https://github.com/libc0607/imx662_modes
