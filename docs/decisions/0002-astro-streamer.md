# ADR-0002: astro_streamer es EL streamer que funciona (vc_num=0 + common pool)

**Fecha:** 2026-08-09
**Estado:** Aceptado

## Contexto

`vi_raw_capture` dejó de funcionar tras un rebuild: fallaba `get_chn_frame 0xa0108016`.
Se creyó primero que el problema era `vc_num=1` (hipótesis: "IMX662 envía por VC1").

## Decisión

`astro_streamer.c` es el streamer de referencia. Usa:
- `pipe_attr.vc_num = 0` (NO 1)
- **Common pool** VB: `vb_exit()` → `vb_set_cfg(max_pool_cnt=1, common_pool blk=6)` →
  `vb_init()` → `vb_get_common_pool_id()` → `init_mod_common_pool(OT_VB_UID_VI)`
- **Sin** `create_pool()` (pool usuario) — eso era lo que rompía la entrega de frames

## Evidencia

- Logs: `vb_exit: 0xa001800d`, `set_cfg: 0xa0018022` (ignorados), `get_common_pool_id: 0x0 cnt=1 id[0]=0`
- **30.0 fps estables** con vc_num=0 + common pool
- Frames 1920x1080 raw12 confirmados por TCP (3.6fps reales = límite 100Mbps, no del sensor)

## Consecuencias

- El determinante NO es el vc_num sino el **tipo de pool VB**
- Test pendiente (documentado): `vi_raw_capture --vc=0` para confirmar que `create_pool()`
  es la causa, no vc_num=1

## Referencias

- `utils/astro_streamer.c`
- `utils/vi_raw_capture.c` (ROTO, no usar)
