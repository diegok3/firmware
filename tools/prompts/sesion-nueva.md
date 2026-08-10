# Prompt: Sesión nueva

Usar al empezar cualquier sesión de trabajo sobre este proyecto.

---

Leé primero estos archivos en orden:
1. `CONTEXT.md` — estado actual (incluye el bug activo del frame blanco)
2. `CHANGELOG.md` — qué se cambió y qué se rompió
3. `docs/decisions/README.md` — decisiones tomadas (y los ADRs relevantes a la tarea)
4. `AGENTS.md` — referencias técnicas, hardware, compilación, deploy
5. `tests/checklist.md` — qué probar antes de declarar que algo funciona

Reglas:
- NO asumas que algo funciona sin verificarlo contra `golden_sets/`
- NO captures frames en el SoC (RAM limitada, OOM). Siempre por TCP al PC
- Si hay un bug activo (ver CONTEXT.md), partí de ahí
- Verificá md5 del binario subido vs compilado local antes de depurar
- Al finalizar, actualizá `CONTEXT.md` y `CHANGELOG.md`

Objetivo de esta sesión: [COMPLETAR]
