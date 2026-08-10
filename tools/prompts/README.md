# tools/prompts — Prompts reutilizables

Prompts de alto nivel para lanzar tareas de forma reproducible.

## Uso

Copiar el contenido de un `.md` como prompt inicial de sesión, o adaptarlo según necesidad.

## Índice

- `sesion-nueva.md` — empezar una sesión leyendo el contexto y el historial correctamente
- `diagnostico-mipi.md` — guía para diagnosticar el bug de frame blanco (MIPI RX)
- `actualizar-docs.md` — actualizar CONTEXT.md + CHANGELOG.md al final de una sesión

## Reglas para escribir un prompt

1. Explicitar QUÉ se quiere lograr (objetivo medible)
2. Explicitar QUÉ archivos leer primero (CONTEXT.md, CHANGELOG.md, ADRs relevantes)
3. Explicitar QUÉ NO tocar (o qué condiciones previas)
4. Exigir verificación contra golden_sets antes de declarar éxito
5. Exigir actualización de docs al final
