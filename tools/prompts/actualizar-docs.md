# Prompt: Actualizar docs al final de sesión

Correr al terminar cualquier sesión de trabajo.

---

Actualizá los documentos de estado del proyecto:

## 1. `CONTEXT.md`
- Actualizá "Última actualización"
- Estado del BUG ACTIVO: ¿se resolvió? ¿cambió el diagnóstico? ¿nuevo paso?
- Estado de los items funcionales

## 2. `CHANGELOG.md`
Agregá entrada nueva con formato:
```md
## YYYY-MM-DD — Resumen corto

**Cambió:** ...
**Se rompió:** ...
**Resuelto / Intentado:** ...
**Lección:** ...
```

## 3. Si hubo una decisión nueva
- Crear `docs/decisions/NNNN-titulo.md`
- Actualizar `docs/decisions/README.md` (tabla)
- Actualizar la tabla de decisiones en `CONTEXT.md`

## 4. Si cambió el comportamiento correcto
- Actualizar/agregar golden set en `golden_sets/`
- Actualizar `golden_sets/README.md`

## 5. Verificación
- [ ] No hay TODOs sin resolver documentados como "hechos" (si no se probó, decirlo)
- [ ] Los archivos están en español (consistente con el proyecto)
