# tools/skills — Skills de opencode del proyecto

Skills locales de opencode para tareas repetitivas del proyecto.
Registradas en `opencode.json` vía `skills.paths: ["tools/skills"]`.

## Cómo crear una skill

Cada skill vive en `tools/skills/<nombre>/SKILL.md` con frontmatter:

```markdown
---
name: <nombre>
description: Qué hace y CUÁNDO usarla (con keywords concretos). "Use when..."
---

# Título

(instrucciones)
```

Requisitos:
- `name` en minúsculas con guiones, coincide con el nombre de la carpeta
- `description` concreta y con keywords de disparo ("Use when...")
- Reiniciar opencode tras crear/editar una skill (no se hot-recargan)

## Índice

- (vacío por ahora — agregar skills cuando se identifiquen tareas repetitivas)

## Ideas de skills a crear

- `diagnosticar-mipi-rx` — pasos del diagnóstico del bug de frame blanco
- `capturar-golden-set` — cómo capturar y registrar un golden set nuevo
- `deploy-streamer` — compilar + subir por base64 + verificar md5 + arrancar
