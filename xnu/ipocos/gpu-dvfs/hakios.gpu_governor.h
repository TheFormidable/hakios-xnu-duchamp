/* SPDX-License-Identifier: MIT
 * The G615's OPP, chosen by its load and held down by its temperature. Pure: no register, no IPI (the caller commits).
 *
 * GPUEB never moves the GPU by itself here (trial 33: OPP 64 of 65, 265 MHz, the whole run). The table is the firmware's
 * own (MediaTek gpufreq_mt6897.h: 1 400 000 kHz at OPP 0 down to 265 000 kHz at 64); only an index is ever asked for.
 * - the busy share of a window (the submits' GPU time over the wall time) aims at GOV_TARGET_PCT: the slowest OPP that
 *   is fast enough for it;
 * - above GOV_RUSH_PCT, the fastest allowed at once: an animation does not wait for steps;
 * - (trial 66) from GOV_ACTIVE_PCT -- an animation -- GOV_FLOOR_OPP at least, at once: a UI frame is one stage in series
 *   with CoreAnimation and the panel, and waits on every millisecond of the GPU (trial 65 asked 284 MHz at 64% busy);
 * - slower only after GOV_DOWN_WINDOWS windows in a row that all want it, GOV_DOWN_STEP OPPs at a time; a window of
 *   GOV_IDLE_NS or more that kept the GPU under 5% busy drops it at once (the screen at rest);
 * - the temperature GPUEB reports caps it (GOV_HOT_*), each cap released GOV_HYSTERESIS_C lower.
 */
#ifndef HAKIOS_GPU_GOVERNOR_H
#define HAKIOS_GPU_GOVERNOR_H
#include <stdint.h>

#define GOV_TARGET_PCT    60u
#define GOV_RUSH_PCT      70u
#define GOV_ACTIVE_PCT    30u
#define GOV_FLOOR_OPP     36           /* 820 MHz in the firmware's table */
#define GOV_DOWN_WINDOWS  5u
#define GOV_DOWN_STEP     12
#define GOV_IDLE_NS       (500ull * 1000000ull)
/* Prepare the first frame for a bounded interval after initial contact.
 * Subsequent moves keep the input activity clock, while measured load sustains the GPU clock. */
#define GOV_TOUCH_OPP     0            /* 1400 MHz, the firmware table's top (the owner: "the 1400 too, not only 820") */
#define GOV_TOUCH_HOLD_NS (50ull * 1000000ull)
/* (trial 112) CoreAnimation presenting GOV_FRAMES_MIN frames or more in the last GOV_FRAMES_NS (20 a second): an animation,
 * held as a touch is. Trial 111: opening the widgets page, frames waited 11-19 ms for the GPU while the window's busy share
 * read 23-26% (a frame is CPU, GPU and panel in series, the GPU idle between its stages), so the governor stepped
 * 1400 -> 1206 -> 1013 MHz in the middle of it, lengthening every frame. The screen at rest (10 a second) does not hold it. */
#define GOV_FRAMES_NS     (250ull * 1000000ull)
#define GOV_FRAMES_MIN    5u
#define GOV_HYSTERESIS_C  5
/* temperature (C) -> the fastest OPP allowed from it */
/* (trial 99, the owner: "Android's or more") 85 C -> OPP 36 (820 MHz), 95 C -> 52 (502 MHz), 105 C -> 64 (265 MHz). Trial 98:
 * the caps of trial 51 (60/70/80 C, the phone hot in the hand) held the GPU at 820 and 502 MHz at 55-70 C, 86% busy, the touch
 * boost cut away. The phone's Android DT trips only at 119 C (gpu1, gpu2, soc_max: live.dtb). */
static const struct { int celsius, best; } gov_hot[] = { {105, 64}, {95, 52}, {85, 36} };

struct gov {
	int cap;          /* the fastest OPP the temperature allows now (0: none held) */
	unsigned down;    /* windows in a row that wanted slower */
	int touch;        /* a touch now or within GOV_TOUCH_HOLD_NS (set by the caller before gov_pick) */
	int previous_touch;
	int interaction, interaction_opp, previous_interaction;
	int dark;         /* (17 Nov) the panel is dark (display off): no animation floor, the load alone decides */
};

/* A live-tunable floor after recent real input. Zero disables it for an unchanged baseline. */
static inline void
gov_configure_interaction(struct gov *g, int hold_ms, int opp, uint64_t since_activity_ns, int n)
{
 if (hold_ms > 1000) hold_ms = 1000;
 g->interaction = hold_ms > 0 && opp >= 0 && opp < n &&
     since_activity_ns < (uint64_t)hold_ms * 1000000ull;
 g->interaction_opp = opp;
}

/* MediaTek's posdiv limits for this GPU: 125 000 .. 1 900 000 kHz; the table must fall from OPP 0 */
static inline int
gov_table_ok(const uint32_t *khz, int n)
{
	if (n < 2 || n > 70) return 0;
	for (int i = 0; i < n; i++) {
		if (khz[i] < 125000u || khz[i] > 1900000u) return 0;
		if (i && khz[i] > khz[i - 1]) return 0;
	}
	return khz[0] > khz[n - 1];
}

static inline int
gov_thermal_cap(struct gov *g, int celsius, int slowest)
{
	int cap = 0;

	for (unsigned i = 0; i < sizeof(gov_hot) / sizeof(gov_hot[0]); i++) {
		/* a cap already held stays until GOV_HYSTERESIS_C below its threshold */
		int at = gov_hot[i].celsius - (g->cap >= gov_hot[i].best ? GOV_HYSTERESIS_C : 0);
		if (celsius >= at) { cap = gov_hot[i].best; break; }
	}
	if (cap > slowest) cap = slowest;
	g->cap = cap;
	return cap;
}

/* Boot-arg gpu_fixed_opp=N: the OPP held whatever the load, only the heat lowering it (the caps above), so that the
 * frequency changes only when the temperature crosses a cap. -1 when the table is not one to trust or N is not in it. */
static inline int
gov_fixed(struct gov *g, const uint32_t *khz, int n, int fixed, int celsius)
{
	if (!gov_table_ok(khz, n) || fixed < 0 || fixed >= n) return -1;
	const int cap = gov_thermal_cap(g, celsius, n - 1);
	return fixed < cap ? cap : fixed;
}

/* The DRAM opp (DVFSRC, 0..9) to ask for with a GPU OPP: the fastest from OPP 36 (820 MHz) up, else no request (0) */
static inline int gov_ddr(int opp) { return opp >= 0 && opp <= 36 ? 9 : 0; }

/* The OPP for the next window, or -1 when the table is not one to trust. cur is the OPP now. */
static inline int
gov_pick(struct gov *g, const uint32_t *khz, int n, int cur, uint64_t busy_ns, uint64_t window_ns, int celsius)
{
	if (!gov_table_ok(khz, n) || !window_ns) return -1;
	if (cur < 0 || cur >= n) cur = n - 1;
	const int cap = gov_thermal_cap(g, celsius, n - 1);
	uint64_t pct = busy_ns >= window_ns ? 100u : busy_ns * 100u / window_ns;
	const int released_touch = g->previous_touch && !g->touch;
	const int released_interaction = g->previous_interaction && !g->interaction;
	g->previous_interaction = g->interaction;
	g->previous_touch = g->touch;
	int want;

	if (pct > GOV_RUSH_PCT) {
		want = cap;
	} else {
		/* the frequency at which this work would have kept the GPU GOV_TARGET_PCT busy */
		uint64_t need = (uint64_t)khz[cur] * pct / GOV_TARGET_PCT;
		want = 0;
		for (int i = n - 1; i >= 0; i--) if (khz[i] >= need) { want = i; break; }
		/* an animation: the interactive floor at least */
		const int floor = GOV_FLOOR_OPP < n - 1 ? GOV_FLOOR_OPP : n - 1;
		if (pct >= GOV_ACTIVE_PCT && !g->dark && want > floor) want = floor;
	}
	if (g->touch) {                                 /* a finger on the glass: up at once, never slower meanwhile */
		const int touch = GOV_TOUCH_OPP < n - 1 ? GOV_TOUCH_OPP : n - 1;
		if (want > touch) want = touch;
		if (want > cur) want = cur;
	}
	if (g->interaction && want > g->interaction_opp) want = g->interaction_opp;
	if (want < cap) want = cap;
	if (cur < cap) return cap;                      /* too hot for where it is: down at once */
	if ((released_touch || released_interaction) && want > cur) { g->down = 0; return want; }
	if (want > cur) {                               /* slower: only when it has wanted it a while, a step at a time */
		if (window_ns >= GOV_IDLE_NS && pct < 5) { g->down = 0; return want; }
		if (++g->down < GOV_DOWN_WINDOWS) return cur;
		g->down = 0;
		return want > cur + GOV_DOWN_STEP ? cur + GOV_DOWN_STEP : want;
	}
	g->down = 0;
	return want;
}
#endif

/* Incremental busy accounting, in the caller's monotonic clock units.
 * Caller seeds last at submission and discards it when the group becomes idle. */
static inline uint64_t gov_busy_delta(uint64_t *last, uint64_t now)
{
 if (now <= *last) return 0;
 uint64_t elapsed = now - *last;
 *last = now;
 return elapsed;
}
