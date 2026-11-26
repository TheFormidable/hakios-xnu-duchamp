# Importado de iPocOS

Archivos de [iPocOS](https://github.com/SuperProgramer64/iPocOS) (commit `7f0308f`, rama iOS 16, `ios16/kernel-hardware.patch`),
el port de SuperProgramer64 al mismo teléfono (POCO X6 Pro, duchamp, MT6897). Solo los que llevan licencia explícita
(SPDX MIT o la cabecera APSL 2.0), copiados **sin cambios**, con su cabecera. El crédito y la licencia de cada uno están
en [NOTICE](../../NOTICE).

**Ninguno está enganchado al kernel.** `duchamp-overlay.py` solo copia `xnu/files/`, así que esta carpeta no cambia el
kernel que compila `mac/hakios-kernel.sh`. Son piezas de referencia para integrarlas a mano: varias llaman a funciones de
archivos de iPocOS que no traen licencia y por eso no están aquí (lo dice el README de cada carpeta).

| Carpeta | Qué es |
| --- | --- |
| `bluetooth/` | transporte BGFSYS/BEIF H4 del Bluetooth del MT6897 |
| `cpufreq/` | lectura pura del controlador DVFS de la CPU y del DVFSRC (DRAM/vcore) |
| `gpu-dvfs/` | gobernador de OPP de la G615 por carga y temperatura, y marcas de progreso de la GPU |
| `display/` | histograma de intervalos de scanout |
| `haptics/` | pines I2C0 GPIO137/138 y reset GPIO22 del motor háptico |
| `boot/` | hueco de RAM y testigo de entrada PSCI (APSL) |
| `console-log/` | formato de los registros de consola en almacenamiento |
| `fingerprint/` | proveedor y buffer de captura del sensor FPC (experimental en iPocOS) |

Lo que **no** se trajo y por qué: ver la tabla final de [NOTICE](../../NOTICE).
