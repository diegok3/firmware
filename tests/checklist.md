# Checklist de pruebas — qué probar antes de cada cambio

> **Regla de oro:** antes de declarar que algo "funciona", ejecutar la prueba
> correspondiente y comparar contra `golden_sets/`. Si no se puede probar en device,
> anotarlo explícitamente en vez de asumir.

## Reglas invariantes (SIEMPRE)

- [ ] Capturar frames SIEMPRE en el PC (`recv_astro.py`/`recv_raw.py`), nunca en el SoC
- [ ] Terminar `astro_streamer` con **SIGTERM** (nunca kill -9 → corrompe VB → reboot)
- [ ] No escribir frames a `/tmp` del SoC (OOM killer)
- [ ] Verificar md5 del binario subido vs el compilado local
  (`md5sum utils/astro_streamer` == `md5sum /root/astro_streamer`)
- [ ] Actualizar `CHANGELOG.md` y `CONTEXT.md` al final de la sesión

## 1. Streamer arranca e inicializa (device)

```bash
rm -f /tmp/astro.log
(setsid /root/astro_streamer 5999 > /tmp/astro.log 2>&1 < /dev/null &)
sleep 9; cat /tmp/astro.log
```

- [ ] `[0] MPI init... OK`
- [ ] `[2] VB init... init: 0x0`
- [ ] `get_common_pool_id: 0x0 cnt=1 id[0]=0`
- [ ] `Chip ID=0x32`, `Phase 1: 149 ok 0 fail`, `Phase 2: 14 ok 0 fail`
- [ ] `[7-9] VI pipeline... VI OK`  (si falla `set_dev_attr 0xa0108011` → estado corrupto, reload completo)
- [ ] `[10] TCP server on port 5999...`

## 2. Frames por TCP (PC)

```bash
python3 utils/recv_astro.py 192.168.1.16 5999
```

- [ ] Llegan frames (header magic "AS" correcto)
- [ ] Tamaño frame = 3,110,400 bytes, 1920×1080, stride 2880
- [ ] **Comparar contra golden set:** el frame debe tener variación (ruido-lluvia),
      **NO** debe ser blanco estático `f0 0f ff`
- [ ] diff_bytes > 0 entre frames consecutivos (hay ruido temporal = sensor vivo)

### Diagnóstico rápido si frame blanco estático

```bash
python3 /tmp/opencode/recv_test.py   # (4 frames, muestra unique + diff)
```

- [ ] `unique=3, b0=f0 b1=0f b2=ff, diff=0` → **BUG del MIPI RX** (ver CONTEXT.md)
- [ ] Registrar SHR/VMAX por comando `E <lines>` y verificar que el frame cambie:
      `E 1239` vs `E 11` — si ambos dan el mismo frame → MIPI RX no recibe datos

## 3. Exposición (comandos de control por TCP)

Conectarse al puerto 5999 y enviar comandos:
- [ ] `E 11` → exp_us ~293 en header
- [ ] `E 1239` → exp_us ~33040 en header
- [ ] `V 1250` → 30fps (periodo ~33ms), `V 10000` → ~3.7fps
- [ ] El frame DEBE cambiar con la exposición (si no → problema de datos, no de control)

## 4. Ganancia

- [ ] `A 1024` vs `A 32768` → el frame debe variar al menos en ruido
- [ ] `D 1024` vs `D 16384` → idem

## 5. Bench de fps (sin TCP)

```bash
/tmp/astro_streamer 5999 --bench 10
```

- [ ] ~30 fps con INCK_SEL=0x03 + DATARATE_SEL=0x05 (VMAX=1250)

## 6. Recuperación tras estado corrupto

- [ ] Si `set_dev_attr 0xa0108011` o "VB pool FAILED" → `load_hisilicon -a -s imx662`
      (reload completo de módulos MPP) y reintentar
- [ ] Si sigue el bug blanco tras reload → **power cycle físico del sensor** (cortar VCC)
      — paso pendiente de probar (bug 2026-08-10)

## 7. Verificación de registros sensor (via comando G)

Conectarse al streamer y enviar `G <reg>` (imprime a stderr en /tmp/astro.log):
- [ ] `G 30DC` → 0x32 (chip ID)
- [ ] `G 3014` → 0x03 (INCK_SEL 27MHz)
- [ ] `G 3015` → 0x05 (DATARATE_SEL 891Mbps)
- [ ] `G 30B0` → 0x00 (test pattern OFF)
- [ ] `G 3000` → 0x00 (streaming)
