# Decisiones (ADRs)

Registro de decisiones de arquitectura (ADR = Architecture Decision Record).
Cada decisión importante que afecte al proyecto va acá con su contexto y justificación.

| ADR | Fecha | Decisión | Estado |
|-----|-------|----------|--------|
| [0001](0001-abandonar-majestic.md) | 2026-08-09 | Abandonar majestic, streamer propio con ISP bypass | Aceptado |
| [0002](0002-astro-streamer.md) | 2026-08-09 | astro_streamer = streamer de referencia (vc_num=0 + common pool) | Aceptado |
| [0003](0003-raw12-lsb-first.md) | 2026-08-10 | Payload raw12 es LSB-first (decode con >>4) | Aceptado |
| [0004](0004-incksel-datarate.md) | 2026-08-08 | INCK_SEL=0x03 + DATARATE_SEL=0x05 = 30fps | Aceptado |

## Cómo agregar una decisión

1. Copiar la plantilla desde el archivo más reciente
2. Nombrar `NNNN-titulo-corto.md` (siguiente número)
3. Llenar: Fecha, Estado (Aceptado/Propuesto/Superado), Contexto, Decisión, Justificación, Consecuencias
4. Agregar fila a la tabla de arriba
5. Actualizar `CONTEXT.md` (tabla de decisiones)
