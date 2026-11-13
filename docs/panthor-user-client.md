# Duchamp: user client, etapa 1

12 de noviembre de 2026. Implementado y probado en host; **sin compilación XNU ni corrida en teléfono**.
La 25b sigue siendo la última corrida de hardware documentada. El binario de kernel adjunto no se ha modificado.

## Qué se implementó

`xnu/files/iokit/Kernel/hakios_Panthor.cpp` crea un servicio distinto de RedwoodMali, que permanece para G52/panfrost.
Se publica después de la prueba GPU y de confirmar TERMINATE del grupo 0, exclusivamente con `mtkgpu_uc=1`.
Se requiere `mtkgpu=1` y los datos de firmware/GPUEB que ya proporciona el loader. La conexión tipo 0 exige root.
Es una restricción de esta etapa de desarrollo; todavía no sirve para ejecutar Mesa/Metal desde aplicaciones.

La cabecera compartida es `xnu/files/bsd/sys/hakios_panthor.h`. Transporte síncrono mediante
`IOConnectCallStructMethod`; selector = número DRM (`0x40 + comando`), nunca el ioctl entero.
Los tamaños son exactos. No se admiten escalares, llamadas asíncronas, descriptores de entrada/salida ni punteros anidados.

| Selector | Operación | Entrada / salida |
| --- | --- | --- |
| 0x80 | GET_ABI propio de HakiOS | 0 / 40 bytes |
| 0x45 | BO_CREATE | 24 / 24 bytes |
| 0x46 | BO_MMAP_OFFSET | 16 / 16 bytes |
| 0x50 | BO_QUERY_INFO | 16 / 16 bytes |
| 0x09 | GEM_CLOSE | 8 / 0 bytes |

Las estructuras BO conservan la disposición de los campos de Panthor consultada; esto **no es compatibilidad Panthor completa**.
GET_ABI informa únicamente capacidad CPU_BO. DEV_QUERY, VM, GROUP, TILER_HEAP, BO_SYNC, labels, MMIO y syncobjs devuelven
Unsupported. No se fingen VMs funcionales, trabajos completados ni sincronización de caché.

BO_CREATE acepta flags 0 o BO_NO_MMAP; exclusive_vm_id debe ser cero. WB_MMAP se rechaza hasta implementar coherencia.
Cada BO usa memoria kernel cableada, compartible, write-combined y alineada al tamaño de página CPU (16 KB en este port),
inicializada con stores escalares de 32 bits. No son páginas del carveout del firmware y no están en tablas GPU.
BO_QUERY_INFO devuelve las flags de creación y extra_flags=0. BO_MMAP_OFFSET devuelve un token `handle << 14`, no una
VA ni dirección física. El adaptador Darwin debe validar alineación y convertir ese token al tipo `handle` de
`IOConnectMapMemory64`. El valor del shift pertenece al transporte HakiOS y no se debe asumir el de Linux.

## Propiedad, vida útil y límites

- Handles locales a cada conexión; no se reciclan durante ella. Hasta 256 creaciones exitosas por conexión.
- 16 MiB por BO, redondeado a página CPU; máximo acumulado de 64 MiB por conexión.
- Cuota global de 128 MiB de backing vivo. Se libera al destruir realmente el descriptor, no al cerrar el handle.
  Incluye mapas que sobreviven a GEM_CLOSE o a la conexión. El lock de presupuesto vive hasta apagar el kernel.
- Un lock por conexión serializa creación, consulta, mapa y cierre. Orden de locks: conexión -> presupuesto.
- GEM_CLOSE invalida el handle, pero los mapas previos conservan su memoria por referencia. Deben desmontarse en usuario.
- clientClose/clientDied/free comparten limpieza idempotente, incluso si start falló parcialmente.
- La validación de salida precede cualquier creación. Fallos de reserva devuelven el presupuesto.
- No se exportan registros, páginas del MCU, direcciones kernel ni direcciones físicas.

## Integración y pruebas

El overlay registra el C++ en `iokit/conf/files`. Se revisaron las firmas contra los headers públicos de
XNU `xnu-8796.121.2`, pero eso no sustituye compilar/enlazar el kernel. El overlay valida todos sus anchors antes de
copiar o modificar archivos; repetirlo no altera el resultado. No es una transacción ante fallos de disco.
El payload de `mac/hakios-kernel.sh` fue regenerado mediante `python3 tools/embed-overlay.py`.

Prueba de host (Linux con compilador C++ y ASan/UBSan):

```sh
python3 tests/run-host-tests.py
```

En entornos que ejecuten bajo ptrace, LeakSanitizer puede fallar antes de informar resultados. En esta sesión se usó:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/run-host-tests.py
```

Pasó con AddressSanitizer y UndefinedBehaviorSanitizer; LeakSanitizer no se ejecutó.
El test compila **el C++ real del cliente**, enlazado a un modelo de IOKit. Comprueba autorización,
rechazo de tamaños/flags, fallo de asignación, cero inicial, aislamiento entre conexiones, tokens obsoletos,
NO_MMAP, cuota global con mapas supervivientes, cuota por conexión, handles agotados y carrera de creación/cierre.
Comprueba contadores de buffers al final. No prueba IOKit real, atributos MMU/cache, el enlace XNU ni el firmware.
También prueba idempotencia del overlay, rechazo sin modificaciones ante anchor incorrecto y payload embebido.

Para la siguiente corrida:

1. Compilar con el flujo habitual `mac/hakios-kernel.sh` del paquete actualizado en el Mac.
2. Conservar el boot_a de respaldo. Usar el flujo de prueba habitual, corto y solo boot_a, agregando
   `mtkgpu_uc=1` a XNU_ARGS junto a los argumentos existentes. No cambiar voltajes, frecuencias ni particiones.
3. Buscar `hakios_panthor: ABI 1 CPU buffers ready; GPU submit unavailable` tras el bring-up completo.
4. Compilar `tools/panthor/hakios-panthor-smoke.c` con el toolchain Darwin/iOS del proyecto y ejecutar **en HakiOS** como root.
   El programa usa IOKitLib; no está compilado ni instalado en este paquete. El SDK objetivo debe tener los headers y
   símbolos de IOKit que usa redwood-drm. Un binario macOS de escritorio no sirve en el teléfono.
5. Esperar `PASS: allocated, zeroed, mapped and CPU-written BO`. Eso acredita el mapa CPU, **no renderizado GPU**.

El dato pendiente para continuar la validación es el log de compilación XNU y luego el log de esa corrida.

## Siguiente etapa: VM y envío

La prueba 25b instala la misma raíz en AS0 y AS1. No reutilizar esa raíz para aplicaciones: contiene firmware y
páginas MCU. Crear raíces privadas por VM, reservar AS0 al MCU y mapear únicamente BOs pertenecientes a la conexión;
caminar segmentos físicos comprobando su longitud, con páginas GPU de 4 KB sobre las de CPU de 16 KB.
Implementar rollback de VM_BIND y mantenimiento de caché/tablas más invalidación TLB con timeout.
No devolver AS ni liberar páginas si no se confirmó la desactivación del hardware.

Después: slots CSG y páginas de cola MCU exclusivas del kernel, ring/sync en la VM del grupo, fences y polling con
referencias a los BOs hasta completar. Un timeout no autoriza a liberar memoria que la GPU aún puede usar.
Cerrar o matar un cliente requiere terminar/resetear de forma comprobable o conservar recursos en cuarentena.
Solo entonces anunciar SUBMIT, implementar DEV_QUERY real y adaptar redwood-drm/Mesa, con pruebas de fallos y dos clientes.

## Fuentes consultadas

- https://github.com/oosexgang/hakios-xnu-kernel/blob/mtk/xnu/files/iokit/Kernel/RedwoodMali.cpp
- https://github.com/torvalds/linux/blob/master/include/uapi/drm/panthor_drm.h (contrato ABI; implementación no copiada)
- https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.121.2/iokit/IOKit/IOUserClient.h
- https://github.com/apple-oss-distributions/xnu/blob/xnu-8796.121.2/iokit/IOKit/IOBufferMemoryDescriptor.h

Archivos nuevos escritos independientemente bajo MIT. No se incorporan fuentes descargadas, firmware ni binarios propietarios.

## Corrección de compilación R1

La primera compilación XNU falló porque IOKit no encontraba `<sys/hakios_panthor.h>`.
El C++ ahora incluye la cabecera privada mediante ruta relativa al source; BSD conserva su include.
El ZIP devuelto tras el error era idéntico al kernel 25b (SHA-256 0f563a751925cf127368c7267cbed506954152e74d4965311ff190ba0a3cbcf2).
El script ahora conserva el kernel anterior en un directorio único `previous-kernel.*`, exige éxito de make
y comprueba fecha, símbolo `_hakios_panthor_start` e identificación ABI antes de empaquetar.
Un fallo incluso de lldbmacros detiene el empaquetado y requiere revisar el log, sin asumir que es inocuo.
Si ya existía un ZIP en el Escritorio, un fallo lo deja intacto: no es un resultado nuevo.
Corrección comprobada con las pruebas de host; pendiente nueva compilación en el Mac.
