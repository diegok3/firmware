# ADR-0001: Abandonar majestic, usar streamer propio con ISP bypass

**Fecha:** 2026-08-09
**Estado:** Aceptado
**Decisión relacionada:** ADR-0002

## Contexto

Majestic (OpenIPC) no lograba entregar video del IMX662:
- VENC timeout (106+) — el encoder H264 no recibía frames
- Exposure stuck en 125ms (8fps) — el ISP nunca convergía
- `cmos_slow_framerate_set` llamado repetidamente — el ISP detectaba frame rate incorrecto
- OOM kill (25MB RAM total, VB pool ~12MB + majestic + kernel ≈ 14MB)
- `mipirx not set lane mode` en dmesg (2x en boot)

Se intentó:
- `data_rate=1` en `[mipi]` → ignorado
- `FullLinesStd=1250` en `[vi_dev]` → no cambia pixel_rate
- `LD_PRELOAD fix_pixel_rate.so` (force 68416666→74250000) → NO resuelve VENC timeout
- Divinus → incompatible (solo Hi3516AV/DV/CV500, usa HI_MPI_* comercial)

## Decisión

Usar un **streamer propio** (`astro_streamer.c`) con **ISP bypass** (`isp_bypass=TD_TRUE`)
sobre la API `ot_mpi_*`, controlando el sensor por I2C directamente. Abandonar majestic.

## Justificación

- `vi_raw_capture` con ISP bypass YA entregaba frames 1920x1080 — el camino bypass funciona
- Control total del sensor (exposición, ganancia, VMAX) vía I2C directo
- Majestic es closed source (Prosperity License) — no hay fuente C para depurar
- Los 25MB de RAM no alcanzan para el pipeline ISP de majestic

## Consecuencias

- Positivas: control total, 30fps exactos posibles, astrophotography (exposición larga)
- Negativas: sin ISP (sin 3A automático, sin demosaic HW, sin H264 en device)
- El raw se manda por TCP al PC para procesar allá

## Referencias

- `general/package/majestic/majestic.mk` (MAJESTIC_SOURCE)
- `utils/fix_pixel_rate.c` (LD_PRELOAD que no resolvió el problema)
