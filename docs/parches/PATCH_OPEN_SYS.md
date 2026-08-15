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

## Documentación Adicional
- Ver `utils/test_init.c` para testear `ss_mpi_sys_init()`
- Ver `CONTEXT.md` y `CHANGELOG.md` para el estado completo del proyecto
- El error `0xa0028022` decompon: MOD_ID=2(SYS), level=ERROR, err_id=0x22(BUSY)
