/* SPDX-License-Identifier: MIT */
#ifndef HAKIOS_FPC_CAPTURE_CORE_H
#define HAKIOS_FPC_CAPTURE_CORE_H

#include <stdbool.h>
#include <stdint.h>

enum fpc_capture_phase {
	FPC_CAPTURE_IDLE,
	FPC_CAPTURE_ACTIVE,
	FPC_CAPTURE_RESTORING,
};

struct fpc_capture_state {
	uint64_t generation;
	uint64_t deadline;
	enum fpc_capture_phase phase;
};

/* Callers serialize state changes and supply the kernel's monotonic clock. */
static inline int
fpc_capture_reserve(struct fpc_capture_state *state, uint64_t deadline, uint64_t *generation)
{
	if (!deadline || !generation) {
		return EINVAL;
	}
	if (state->phase != FPC_CAPTURE_IDLE) {
		return EBUSY;
	}
	if (!++state->generation) {
		++state->generation;
	}
	state->deadline = deadline;
	state->phase = FPC_CAPTURE_ACTIVE;
	*generation = state->generation;
	return 0;
}

static inline bool
fpc_capture_is_active(const struct fpc_capture_state *state, uint64_t generation, uint64_t now)
{
	return state->phase == FPC_CAPTURE_ACTIVE && state->generation == generation && now < state->deadline;
}

static inline int
fpc_capture_stop(struct fpc_capture_state *state, uint64_t generation, int (*restore)(void *), void *hardware)
{
	if (state->phase == FPC_CAPTURE_IDLE || state->generation != generation) {
		return ESTALE;
	}
	state->phase = FPC_CAPTURE_RESTORING;
	int error = restore(hardware);
	if (!error) {
		state->phase = FPC_CAPTURE_IDLE;
		state->deadline = 0;
	}
	return error;
}

static inline int
fpc_capture_expire(struct fpc_capture_state *state, uint64_t now, int (*restore)(void *), void *hardware)
{
	if (state->phase == FPC_CAPTURE_IDLE ||
	    (state->phase == FPC_CAPTURE_ACTIVE && now < state->deadline)) {
		return 0;
	}
	return fpc_capture_stop(state, state->generation, restore, hardware);
}

#endif
