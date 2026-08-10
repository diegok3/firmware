# Prompt: Diagnosticar MIPI RX (bug de frame blanco)

Guía para el bug activo: frame 100% blanco estático `f0 0f ff` tras reboot.

---

## Contexto

Ver `CONTEXT.md` → BUG ACTIVO (2026-08-10). El streamer entrega 30fps pero el frame
es `f0 0f ff` (unique=3, diff=0). No responde a exposición ni ganancia. El reload de
módulos NO lo arregla.

## Hipótesis a verificar (en orden)

1. **`OT_MIPI_ENABLE_MIPI_CLOCK` faltante** — el streamer no lo llama (ioctl 0x0c).
   El driver de resume usa: enable_mipi_clock → reset_mipi → set_attr → unreset_mipi.
   Agregar al init y probar.
2. **Power cycle físico del sensor** — cortar VCC del sensor y volver a dar (único
   paso no probado). Si el usuario puede hacerlo, probar.
3. **Orden del reset MIPI vs sensor** — probar reset_mipi + enable_clock antes/después
   del init del sensor.

## Procedimiento de prueba

1. Compilar, subir (base64), verificar md5
2. Arrancar streamer, revisar `/tmp/astro.log` (VI OK requerido)
3. Capturar frames en PC: `python3 /tmp/opencode/recv_test.py`
4. Comprobar: `unique` debe ser >3 y `diff_bytes` >0 (no blanco estático)
5. Comparar contra `golden_sets/imx662-lensless/` (ruido-lluvia correcto)

## Verificación final

- [ ] frame tiene variación temporal (diff > 0)
- [ ] responde a `E <lines>` (exposición)
- [ ] md5 del binario coincide
- [ ] Si sigue roto: documentar en CONTEXT.md y CHANGELOG.md
