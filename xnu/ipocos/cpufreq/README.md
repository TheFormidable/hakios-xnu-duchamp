# DVFS de la CPU y DVFSRC (iPocOS, MIT)

Cabeceras puras (sin registros ni efectos): `hakios.mtk_cpufreq.h` interpreta el controlador `mediatek,cpufreq-hw`
(dominios de 0x120 bytes, tabla en +0x0, ENABLE +0x84, PERF_STATE +0x88, HW_STATE +0x8c), lo mismo que ya lee
`CPUFREQ_SHOW` en el loader. `hakios.dvfsrc.h` interpreta el nivel de DRAM/vcore. Compilan solas (`cc -fsyntax-only`).
El driver que las usa en iPocOS (`hakios.cpufreq.c`) no tiene licencia.
