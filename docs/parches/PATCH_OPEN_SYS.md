# Parche Binario open_sys.ko - Error SYS_BUSY (0xa0028022)

## Propósito
Parcheo binario del driver `open_sys.ko` para reemplazar el error `OT_ERR_SYS_BUSY` (0xa0028022) por éxito (0x00000000) en las llamadas `ioctl` al driver kernel SYS.

## Contexto
El error `0xa0028022` causaba que `ss_mpi_sys_init()` y `astro_streamer` fallaran con "SYS init FAIL (0xa0028022)". El driver kernel SYS tiene un flag interno de "ya inicializado" que, cuando está set, hace que el ioctl retorne BUSY en lugar de éxito.

## Qué Hace el Parche
Reemplaza 4 rangos de 4 bytes cada uno en `open_sys.ko` donde aparece el patrón `0xa0028022` (en little-endian: bytes `0x22 0x80 0x02 0xa0`) por ceros (`0x00000000`).

Esto hace que el driver kernel SYS siempre retorne éxito en lugar de BUSY, permitiendo que `ss_mpi_sys_init()` complete correctamente.

## Offsets Afectados (offsets byte en el archivo binary)
Los siguientes offsets fueron parcheados (mostrando contenido antes y después):

| Offset (hex) | Byte Offset | Contenido Antes | Después |
|-------------|-------------|-----------------|---------|
| 0x27f4 | 10228 | 0xa0028022 | 0x00000000 |
| 0x28a4 | 10404 | 0xa0028022 | 0x00000000 |
| 0x2970 | 10608 | 0xa0028022 | 0x00000000 |
| 0x2a40 | 10816 | 0xa0028022 | 0x00000000 |

## Cómo Aplicar el Parche

### 1. Copiar el archivo original
```bash
cp output/target/lib/modules/5.10.221/hisilicon/open_sys.ko /tmp/opencode/open_sys.ko.bak
```

### 2. Aplicar el parche
```bash
python3 -c "
import struct
with open('/tmp/opencode/open_sys.ko.bak', 'rb') as f:
    data = bytearray(f.read())
byte_offsets = [10228, 10404, 10608, 10816]
for boffset in byte_offsets:
    for i in range(4):
        data[boffset + i] = 0
with open('/tmp/opencode/open_sys.ko patched.ko', 'wb') as f:
    f.write(data)
print(f'Parcheo completado: {4} rangos reemplazados')
"

### 3. Instalar el módulo parcheado
```bash
# En el device:
base64 -d /tmp/lib.b64 > /lib/modules/5.10.221/hisilicon/open_sys.ko
# O copiar directamente:
cp /ruta/al/parcheado.ko /lib/modules/5.10.221/hisilicon/open_sys.ko
# Luego reiniciar el módulo:
rmmod open_sys && insmod /lib/modules/5.10.221/hisilicon/open_sys.ko
```

## Limitaciones y Hallazgos

### Por Qué El Parche No Tuvo Efecto Inicial
1. El módulo `open_sys.ko` ya estaba **cargado en el kernel** con los valores originales
2. `rmmod` + `insmod` causaba timeouts en la sesión SSH
3. El sistema finalmente funcionó después de un **reinicio físico** del device, lo que sugiere que el flag de "ya inicializado" se reseteara en el boot

### Estado Actual
- El parcheo binario es una solución **temporal/workaround**
- El sistema funciona correctamente después de reinicios
- Se recomienda investigar la raíz profunda del flag "ya inicializado" en el driver kernel
- **No es necesario mantener el parche** si el sistema funciona con los reinicios normales

## Causa Raíz (investigado 2026-08-15, source del driver)

El driver `open_sys` está en `hi_sys.o` (blob cerrado; el wrapper `init/hi3516cv6xx/sys_init.c` es fuente). El error `0xa0028022` se genera en exactamente **una función** del blob:

- **`sys_ioctl_user_init`** (los 4 offsets del parche son los 4 casos de retorno
  de esa función para init/exit/audio-init/audio-exit)

### Flag de estado "ya inicializado"
Hay un global del kernel `g_sys + 0x4bc` (y `+0x4c0` para audio) que actúa como flag de boot:

| Valor | Significado |
|-------|-------------|
| 0 | ya inicializado (init completó OK) |
| 1 | busy (exit previo no completó) |
| 2 | no inicializado (estado por defecto en `sys_do_mod_init`) |

`sys_ioctl_user_init`:
- si flag==1 → **retorna `0xa0028022`** (SYS_BUSY)
- si flag==2 → corre `sys_ctl()` + `cmpi_init_modules()`; si la init de módulos
  falla → **también retorna `0xa0028022`**
- si flag==0 → "sys init again!" → success (idempotente)

`sys_ioctl_user_exit` → si algún módulo no se detiene en 500ms (10 iter × 50ms),
setea flag=1 y retorna SYS_BUSY.

**Dos caminos al SYS_BUSY:**
1. flag quedó en 1 (exit previo incompleto — mata el proceso en medio del teardown)
2. `cmpi_init_modules()` devuelve error (el más probable tras reboot limpio)

`cmpi_init_modules` vive en `hi_base.o` (open_base); su wrapper `init/hi3516cv6xx/base_init.c`
llama `comm_init()`. Éste itera todos los módulos registrados (`cmpi_register_module`) y
llama su `init`. Si un módulo registrado no tiene su init disponible (ej. `open_cipher`
o `open_km` sin blob para V5) puede fallar. El segundo gate es el byte de estado por
módulo en `[func+0x1400]` que chequea `sys_drv_drv_ioctrl` y `sys_do_mod_init`.

## Cómo distinguir la raíz real (diagnóstico)

Correr `astro_streamer` **sin parche** y mirar el log del kernel:
- `sys is busy!` → flag quedó en 1 → limpiar con **reboot físico** (o hacer teardown
  limpio del proceso anterior con SIGTERM, nunca kill -9)
- `init modules failed!` → `cmpi_init_modules()` falló → falta un módulo registrado
  (probablemente `open_cipher`/`open_km`, blobs V5 cerrados que no existen)

## Documentación Adicional
- Ver `utils/test_init.c` para testear `ss_mpi_sys_init()`
- Ver `CONTEXT.md` y `CHANGELOG.md` para el estado completo del proyecto
- El error `0xa0028022` descompone: MOD_ID=2(SYS), level=ERROR, err_id=0x22(BUSY)
