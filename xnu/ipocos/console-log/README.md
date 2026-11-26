# Registros de consola (iPocOS, MIT)

`hakios.emmc_log.h`: registros de 4096 bytes (magic, secuencia, corrida, posición, longitud, CRC32) para guardar la
consola del kernel en almacenamiento y leerla después aunque un registro quede a medias. En iPocOS el mismo archivo está
en `bsd/dev/` y `bsd/sys/`. Necesita `<stdint.h>` antes de incluirlo.
