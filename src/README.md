# src — Código fuente

Código fuente del proyecto. 

> **Nota de organización:** actualmente el código vive en `utils/` (streamer y
> herramientas de diagnóstico compiladas para el device) y en el árbol de build
> (drivers de sensor modificados en `general/` y `output/build/...`).

## Ubicación actual del código

| Qué | Dónde |
|-----|-------|
| Streamer de referencia | `utils/astro_streamer.c` |
| Receiver OpenCV (PC) | `utils/recv_astro.py`, `utils/recv_raw.py` |
| Herramientas I2C/MIPI diagnóstico | `utils/i2c_*.c`, `utils/vi_raw_capture.c` |
| Driver sensor IMX662 (build tree) | `output/build/hisilicon-opensdk-ff20187b/libraries/sensor/hi3516cv6xx/sony_imx662/` |
| Overlay y scripts | `general/overlay/`, `general/package/hisilicon-osdrv-hi3516cv6xx/` |

## Convención

- El código que se ejecuta EN el device se compila con el toolchain de buildroot
  (ver `AGENTS.md` → Compilación) y se sube por SSH base64
- El código que corre EN el PC (receive, análisis, docs) es Python
- Antes de mover código a `src/`, documentar la decisión en `docs/decisions/`
