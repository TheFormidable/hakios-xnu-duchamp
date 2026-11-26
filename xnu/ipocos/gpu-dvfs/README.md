# Frecuencia de la Mali-G615 (iPocOS, MIT)

`hakios.gpu_governor.h`: elige el OPP de la G615 (tabla de `gpufreq_mt6897.h`, 1400 MHz en OPP 0 a 265 MHz en OPP 64)
por carga y lo limita por temperatura. Es puro: quien lo llama manda el índice al GPUEB. En iPocOS el GPUEB no movía la
GPU solo (se quedaba en 265 MHz), y esto lo arregló. Encaja en `redwood_mtkgpu.c`, que ya habla con el GPUEB.
`hakios.gpu_mark.h`: marcas del progreso de la GPU para que el panel espere solo su cuadro (boot-arg `gpu_mark=1`).
