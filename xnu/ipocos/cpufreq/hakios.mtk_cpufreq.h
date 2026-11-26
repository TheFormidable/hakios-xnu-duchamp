/* SPDX-License-Identifier: MIT
 * The MT6897's CPU DVFS performance controller ("mediatek,cpufreq-hw", soc/performance-controller@11bc00), pure.
 * Layout from Linux drivers/cpufreq/mediatek-cpufreq-hw.c: per domain (0x120 bytes) the frequency table at +0x0 (32
 * rows of 4 bytes, MHz in bits 11:0, the first row the fastest; a repeated frequency ends it), ENABLE +0x84 (bit 0: the
 * hardware does DVFS), PERF_STATE +0x88 (the row software asks for), HW_STATE +0x8c (what the hardware is doing).
 * Nothing on this phone asked for a CPU frequency (trial 39: ~34 ms of an 80 ms frame on the CPU outside metal2vulkan).
 */
#ifndef HAKIOS_MTK_CPUFREQ_H
#define HAKIOS_MTK_CPUFREQ_H
#include <stdint.h>

#define MTK_CPUFREQ_ROWS        32
#define MTK_CPUFREQ_ENABLE      0x84u
#define MTK_CPUFREQ_PERF_STATE  0x88u
#define MTK_CPUFREQ_HW_STATE    0x8cu
#define MTK_CPUFREQ_DOMAIN_SIZE 0x120u

/* the table's rows in MHz into mhz[], how many (Linux: a frequency equal to the last ends it) */
static inline int
mtk_cpufreq_lut(const volatile uint32_t *domain, uint32_t mhz[MTK_CPUFREQ_ROWS])
{
	int n = 0;
	uint32_t previous = 0;

	for (int i = 0; i < MTK_CPUFREQ_ROWS; i++) {
		uint32_t f = domain[i] & 0xfffu;
		if (f == 0 || (n > 0 && f == previous)) break;
		mhz[n++] = f;
		previous = f;
	}
	return n;
}

/* a domain to ask: two rows or more, each 100..4000 MHz, falling from the first, and the row asked now one of them.
 * ENABLE is not required: on this phone (trial 40) the A510 domain reads 0x101 and both A715 domains 0, while all three
 * hold MediaTek's tables (2200..480, 3200..400, 3350..400 MHz) and a row asked (0, 20, 22). */
static inline int
mtk_cpufreq_sane(const volatile uint32_t *domain, const uint32_t *mhz, int n)
{
	if (n < 2 || domain[MTK_CPUFREQ_PERF_STATE / 4] >= (uint32_t)n) return 0;
	for (int i = 0; i < n; i++) {
		if (mhz[i] < 100 || mhz[i] > 4000) return 0;
		if (i && mhz[i] >= mhz[i - 1]) return 0;
	}
	return 1;
}

/* The governor (schedutil-like). Trial 44 (every domain held at its top row) reached 68-73 C. Each domain follows the
 * load of its busiest core, aiming at MTK_CPUFREQ_TARGET_PCT busy: up at once, down after MTK_CPUFREQ_DOWN_SAMPLES samples
 * that all want it; capped by heat (GPUEB's temperature, the SoC's): 70 C -> 50%, 80 C -> 30% of the top, each released
 * 5 C lower; MTK_CPUFREQ_PCT_UNKNOWN of the top while no temperature is known. Trial 97: at 56-57 C the old 60 C cap
 * (released at 55) held the big cores at 1.0-1.7 GHz of 3.2 with 73% load, CoreAnimation's frames waiting on them. */
#define MTK_CPUFREQ_PCT_NORMAL 100u
#define MTK_CPUFREQ_PCT_UNKNOWN 75u
#define MTK_CPUFREQ_TARGET_PCT 80u
#define MTK_CPUFREQ_RUSH_PCT 90u
#define MTK_CPUFREQ_DOWN_SAMPLES 3u
/* (GTA, 17 Nov) a step down goes at most to MTK_CPUFREQ_DOWN_PCT of the current clock: one sample after a burst at the
 * top used to drop a domain straight to its slowest row, where 78% busy (below the 80% target) held it; the A715s spent
 * 47% of a game's samples at 400-600 MHz with 81% load and 39% at 3200, nothing between, every frame a different length. */
#define MTK_CPUFREQ_DOWN_PCT 60u
/* (trial 99, the owner: "Android's or more") 85 C -> 50%, 95 C -> 30%; the phone's DT trips only at 119 C */
static const struct { int celsius; uint32_t pct; } mtk_cpufreq_hot[] = { {95, 30}, {85, 50} };

/* Initial-contact interval configured by this POCO's Android powercontable.xml.
 * Keep the real measured utilization throughout the interval. */
#define MTK_CPUFREQ_TOUCH_HOLD_NS 50000000ull
#define MTK_CPUFREQ_INTERACTION_HOLD_NS 500000000ull

/* the slowest row at least pct% of the top (the first row), or the last row */
static inline int
mtk_cpufreq_row_for(const uint32_t *mhz, int n, uint32_t pct)
{
	if (n < 1) return 0;
	uint32_t need = mhz[0] * pct / 100u;
	for (int i = n - 1; i >= 0; i--) if (mhz[i] >= need) return i;
	return 0;
}

/* the slowest row of at least want_mhz, or the first row */
static inline int
mtk_cpufreq_row_mhz(const uint32_t *mhz, int n, uint32_t want_mhz)
{
	for (int i = n - 1; i >= 0; i--) if (mhz[i] >= want_mhz) return i;
	return 0;
}

/* the share of the top the temperature allows; *state keeps the share held, for the 5 C hysteresis */
static inline uint32_t
mtk_cpufreq_thermal_pct(int celsius, uint32_t *state)
{
	uint32_t pct = MTK_CPUFREQ_PCT_NORMAL;
	for (unsigned i = 0; i < sizeof(mtk_cpufreq_hot) / sizeof(mtk_cpufreq_hot[0]); i++) {
		int at = mtk_cpufreq_hot[i].celsius - (*state && *state <= mtk_cpufreq_hot[i].pct ? 5 : 0);
		if (celsius >= at) { pct = mtk_cpufreq_hot[i].pct; break; }
	}
	*state = pct == MTK_CPUFREQ_PCT_NORMAL ? 0 : pct;
	return pct;
}

/* the row for the next sample: cur the row now, util the busiest core's busy % over the last one, cap_pct the share of
 * the top allowed, *down the samples in a row that wanted a slower row */
static inline int
mtk_cpufreq_pick(const uint32_t *mhz, int n, int cur, uint32_t util, uint32_t cap_pct, uint32_t *down)
{
	if (n < 1) return 0;
	if (cur < 0 || cur >= n) cur = n - 1;
	const int cap = mtk_cpufreq_row_for(mhz, n, cap_pct);
	int want;

	if (util >= MTK_CPUFREQ_RUSH_PCT) {
		want = cap;
	} else {
		uint64_t need = (uint64_t)mhz[cur] * util / MTK_CPUFREQ_TARGET_PCT;
		want = 0;
		for (int i = n - 1; i >= 0; i--) if (mhz[i] >= need) { want = i; break; }
	}
	if (want < cap) want = cap;
	if (cur < cap) { *down = 0; return cap; }        /* faster than the heat allows: down at once */
	if (want > cur) {                               /* slower: only once it has wanted it a while, and a step at a time */
		if (++*down < MTK_CPUFREQ_DOWN_SAMPLES) return cur;
		*down = 0;
		int step = mtk_cpufreq_row_mhz(mhz, n, mhz[cur] * MTK_CPUFREQ_DOWN_PCT / 100u);
		if (step <= cur) step = cur + 1;
		if (want > step) want = step;
		return want;
	}
	*down = 0;
	return want;
}

/* Prepare initial contact briefly, then sustain the POCO's launch minimums while input is recent.
 * gpu26's touch-only rows left the prime at a 500 MHz median during measured UI work.
 * Keep measured load and thermal ceilings in control above these minimums. */
/* (GTA, 17 Nov) An app (not the compositor) that gave the GPU work in the last app_hold_ns is drawing: a game's frame is
 * its CPU threads then the GPU, at 20-30 ms a frame the 20 ms load samples see bursts and gaps, and a frame begun at 400
 * MHz took 8 times one at 3200. The big domains (1, 2) keep at least app_floor_mhz meanwhile (sysctls
 * kern.hakios.cpu_app_floor_mhz, kern.hakios.cpu_app_hold_ms; 0 turns it off); load still raises them, heat caps them. */
static inline int
mtk_cpufreq_pick_interactive(const uint32_t *mhz, int n, int cur, uint32_t util,
    uint32_t cap_pct, uint32_t *down, unsigned domain, uint64_t since_touch_ns,
    uint64_t since_activity_ns, uint64_t since_app_gpu_ns, uint64_t app_hold_ns, uint32_t app_floor_mhz)
{
	int next = mtk_cpufreq_pick(mhz, n, cur, util, cap_pct, down);
	static const uint32_t minimum_mhz[] = { 1600, 2200, 2200 };
	if (n < 1 || domain >= 3) return next;
	int floor = n - 1;
	if (since_touch_ns < MTK_CPUFREQ_TOUCH_HOLD_NS) {
		floor = 0;
	} else if (since_activity_ns < MTK_CPUFREQ_INTERACTION_HOLD_NS) {
		floor = mtk_cpufreq_row_mhz(mhz, n, minimum_mhz[domain]);
	}
	if (domain >= 1 && app_floor_mhz && since_app_gpu_ns < app_hold_ns) {
		const int app = mtk_cpufreq_row_mhz(mhz, n, app_floor_mhz);
		if (app < floor) floor = app;
	}
	if (floor == n - 1) return next;
	const int cap = mtk_cpufreq_row_for(mhz, n, cap_pct);
	if (floor < cap) floor = cap;
	if (next > floor) { *down = 0; next = floor; }
	return next;
}
#endif
