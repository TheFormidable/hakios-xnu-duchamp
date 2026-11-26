# Huella FPC (iPocOS, MIT, experimental)

`hakios.fpc_provider.c` con `hakios.fpc.h` / `hakios.fpc_provider.h`: el proveedor de capturas del sensor óptico FPC.
`fpc_capture_core.h`: reserva y estado del buffer de captura. En iPocOS la última versión se quedaba colgada al
desbloquear. El transporte SPI y los parches del panel (`ios16/fingerprint/*.patch`) no tienen licencia.
