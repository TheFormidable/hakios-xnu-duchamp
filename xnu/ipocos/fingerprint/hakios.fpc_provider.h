/* SPDX-License-Identifier: MIT */
#ifndef HAKIOS_FPC_PROVIDER_H
#define HAKIOS_FPC_PROVIDER_H

#include <sys/hakios.fpc.h>

/* Kernel-only contract for a platform provider. The provider owns MITEE
 * admission and must bound open to the monotonic deadline. Any non-null
 * session transferred to the caller must be valid for close, even on error. */
struct hakios_fpc_provider {
	int (*open)(void *context, uint64_t deadline, void **session);
	int (*close)(void *context, void *session);
};

struct hakios_fpc_probe_result {
	bool opened;
	int prepare_error;
	int open_error;
	int snapshot_error;
	int close_error;
	int release_error;
	struct hakios_fpc_status status;
};

int hakios_fpc_probe_provider(const struct hakios_fpc_provider *provider, void *context,
    uint32_t duration_ms, struct hakios_fpc_probe_result *result);

#endif
