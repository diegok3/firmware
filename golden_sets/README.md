# Golden sets — ejemplos de lo que debe seguir funcionando

> Un golden set es la referencia objetiva de "esto funciona". Antes de declarar que
> un cambio no rompió nada, comparar contra estos ejemplos.

## imx662-lensless/ — COMPORTAMIENTO CORRECTO ✅

Capturas del 2026-08-09 20:15-20:55 con `astro_streamer` + decode **LSB-first**.

**Qué muestran:** ruido-lluvia (mean ~140/255, frame-diff=99, 18-248 valores únicos).
Esto es el comportamiento **correcto** para un sensor IMX662 SIN lente:
- Campo plano brillante con ruido temporal (cada frame difiere)
- NO hay estructura espacial (no hay lente que forme imagen)
- Los bits bajos son ruido real del sensor

| Archivo | Descripción |
|---------|-------------|
| `dec_correct.png` | Decode LSB-first correcto (12-bit → 16-bit → PNG) |
| `dec8_correct.png` | Decode LSB-first 8-bit |
| `gray8.png` | Grayscale |
| `A_msb.png` / `B_lsb.png` / `C_8bit.png` | Variantes de decode |

**Criterio de aceptación:** un frame nuevo se ve "similar" (mismo tipo de ruido,
mismo rango ~140), y **varía entre frames**.

## white-bug/ — BUG CONOCIDO ❌

Capturas del 2026-08-10 (tras reboot) con el mismo binario.

| Archivo | Descripción |
|---------|-------------|
| `current.raw` | Frame raw12 3MB, patrón `f0 0f ff` repetido (100% estático) |

**Qué muestra:** frame 100% blanco estático, no responde a exposición/ganancia.
**Diagnóstico:** MIPI RX no recibe datos válidos del sensor. Ver `CONTEXT.md` → BUG ACTIVO.

## Cómo agregar un golden set

1. Capturar el ejemplo con `recv_astro.py`/`recv_raw.py` en el PC
2. Guardar en `golden_sets/<nombre-significativo>/`
3. Documentar en este README: qué muestra, cuándo se capturó, criterio de aceptación
4. Si es una regresión, guardarla en su propia carpeta (`*-bug/`) para referencia
