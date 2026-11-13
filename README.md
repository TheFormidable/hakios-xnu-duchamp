# hakios-xnu-duchamp

El kernel de HakiOS para el **POCO X6 Pro 5G (duchamp, MT6897)**: lo que este teléfono añade al XNU del árbol común
([hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel), rama `mtk`, xnu-8796.121.2). El loader, las imágenes y
los scripts de flasheo están en el repositorio del dispositivo, [hakios-device-duchamp](https://github.com/TheFormidable/hakios-device-duchamp).

## Qué hay

| Archivo | Qué es | Boot-arg |
| --- | --- | --- |
| `xnu/files/bsd/dev/redwood_mtkufs.c` | el UFS: la raíz del sistema en la partición `xnusys` | `ufs_root=1` |
| `xnu/files/bsd/dev/redwood_mtktouch.c` | el tacto (Goodix GT9916 / FocalTech) | `mtktouch=` |
| `xnu/files/bsd/dev/redwood_mtkgpu.c` | la Mali-G615: encendido por el GPUEB, firmware CSF, arranque del MCU, una cola de prueba | `mtkgpu=1` |
| `xnu/files/iokit/Kernel/hakios_Panthor.cpp` | el user client de la GPU, etapa 1: buffers de CPU (solo root) | `mtkgpu_uc=1` |
| `xnu/duchamp-overlay.py` | copia `xnu/files/` sobre el árbol y engancha cada driver (también el arreglo de `io_map.c` y el núcleo 0 aparcado) | |
| `mac/hakios-kernel.sh` | compila el kernel en un Mac, con el overlay metido dentro | |
| `tools/embed-overlay.py` | vuelve a meter el overlay en `mac/hakios-kernel.sh`: córrelo tras cambiar algo en `xnu/` | |
| `tests/run-host-tests.py` | pruebas en el host del user client (modelo de IOKit) y del overlay | |
| `tools/panthor/hakios-panthor-smoke.c` | la prueba del user client desde iOS | |

Estado (12 de noviembre de 2026): iOS 16.5 arranca desde `xnusys` con 7 núcleos y tacto; la GPU llega al paso 6 de 6 dentro
de iOS (corrida 25b), y la etapa 1 del user client dio PASS desde iOS (corrida 27). Lo siguiente está en
[docs/panthor-user-client.md](docs/panthor-user-client.md).

## Compilar

En el Mac: `bash mac/hakios-kernel.sh`. Clona el árbol común, aplica su parche y este overlay, compila y deja
`hakios-kernel-duchamp.zip` en el Escritorio. El kernel compilado no se sube a este repositorio.

Pruebas en Linux: `python3 tests/run-host-tests.py` (con ASan/UBSan).

Licencias: ver [NOTICE](NOTICE). Lo que este repositorio no lleva nunca: [LEGAL.md](LEGAL.md).
