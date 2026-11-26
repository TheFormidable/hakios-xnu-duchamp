# Bluetooth del MT6897 (iPocOS, MIT)

`hakios.mtk_bt.c`: encendido de BGFSYS y transporte H4 por BEIF (el firmware lo deja cargado LK), expuesto como
dispositivo de caracteres. `hakios.mtk_bt_beif.h`: la disposición de BEIF v0. `hakios.mtk_bt.cpp`: publica el
dispositivo en IOKit. Original: `bsd/dev/` e `iokit/Kernel/` del parche de iPocOS.

Para integrarlo falta lo que iPocOS no da con licencia: `hakios_mtk_conn_bt_status()` (en `hakios.mtk_conn.c`, el
encendido de CONNSYS) y `loader_log_line()`. En iPocOS con esto se oyó audio A2DP.
