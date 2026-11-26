# Ritmo del scanout (iPocOS, MIT)

`scan_pacing.h`: histograma acotado de los intervalos entre scanouts (bins de 1 ms, el 255 desborde), para medir los
fps reales del panel. El presentador que lo usa en iPocOS (`MiniIOMobileFramebuffer.cpp`) no tiene licencia.
