# ADR-0003: Payload raw12 es LSB-first (decode con >>4)

**Fecha:** 2026-08-10
**Estado:** Aceptado

## Contexto

Las imágenes del streamer parecían "sin correlación espacial". Se analizó la estructura
de bytes del triplet `[b0,b1,b2]` (por cada 2 píxeles) y se encontró:
- `b0` siempre múltiplo de 16 (nibble bajo = 0)
- `b1` siempre < 16 (nibble alto = 0)
- `b2` rango completo

Esto es la firma de datos 8-bit (`v<<4`) en contenedor 12-bit **LSB-first**.

## Decisión

El decode correcto es:
```python
v0 = ((b1 & 0xF) << 4) | (b0 >> 4)
v1 = b2
```
(equivalente: 12-bit LSB >> 4). Antes se decodificaba MSB-first (WRONG).

## Implementación

- `utils/recv_astro.py:raw12_to_uint16`
- `utils/recv_raw.py:unpack_raw12`

## Nota importante

A pesar del decode correcto, la imagen mostraba campo plano brillante sin correlación
espacial (ch2/cv2 < 0.4). El usuario aclaró que el **ruido-lluvia es CORRECTO** porque el
sensor no tiene lente. Las imágenes correctas están en `golden_sets/imx662-lensless/`.

## Golden set

`golden_sets/imx662-lensless/dec_correct.png`, `dec8_correct.png`, `gray8.png` — capturas
del 2026-08-09 20:15-20:55 con decode LSB-first.
