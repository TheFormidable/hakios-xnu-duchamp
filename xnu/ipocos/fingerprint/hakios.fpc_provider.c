/* SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <sys/errno.h>
#include <sys/hakios.fpc_provider.h>

int
hakios_fpc_probe_provider(const struct hakios_fpc_provider *provider, void *context,
    uint32_t duration_ms, struct hakios_fpc_probe_result *result)
{
	if (!result) {
		return EINVAL;
	}
	*result = (struct hakios_fpc_probe_result){0};
	if (!provider || !provider->open || !provider->close) {
		return ENOTSUP;
	}

	uint64_t generation, deadline;
	/* Start the provider's budget before preparation so it expires at or before
	 * the hardware reservation. The driver has its own expiry timer. */
	clock_interval_to_deadline(duration_ms, NSEC_PER_MSEC, &deadline);
	result->prepare_error = hakios_fpc_prepare(duration_ms, &generation);
	if (result->prepare_error) {
		return result->prepare_error;
	}

	void *session = NULL;
	result->open_error = mach_absolute_time() >= deadline ? ETIMEDOUT :
	    provider->open(context, deadline, &session);
	if (!result->open_error && !session) {
		result->open_error = EPROTO;
	}
	if (!result->open_error) {
		result->opened = true;
		struct hakios_fpc_status status;
		result->snapshot_error = mach_absolute_time() >= deadline ? ETIMEDOUT :
		    hakios_fpc_snapshot(generation, &status);
		if (!result->snapshot_error) {
			result->status = status;
		}
	}

	if (session) {
		result->close_error = provider->close(context, session);
	}
	result->release_error = hakios_fpc_cancel(generation);

	if (result->open_error) {
		return result->open_error;
	}
	if (result->snapshot_error) {
		return result->snapshot_error;
	}
	if (result->close_error) {
		return result->close_error;
	}
	return result->release_error;
}
