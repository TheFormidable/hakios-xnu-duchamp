/* SPDX-License-Identifier: MIT */
#ifndef HAKIOS_FPC_H
#define HAKIOS_FPC_H

#include <stdbool.h>
#include <stdint.h>

struct hakios_fpc_status {
	bool irq_level;
	bool irq_pending;
	bool panel_ready;
};

/* Kernel clients call from a sleepable context. The generation owns the hardware
 * until cancel or expiry. Panel illumination has its own controller and timeout. */
int hakios_fpc_prepare(uint32_t duration_ms, uint64_t *generation);
int hakios_fpc_snapshot(uint64_t generation, struct hakios_fpc_status *status);
int hakios_fpc_cancel(uint64_t generation);
bool hakios_udfps_capture_ready(void);
bool hakios_udfps_is_opted_in(void);

#endif
