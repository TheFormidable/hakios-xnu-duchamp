/* SPDX-License-Identifier: MIT
 * MediaTek DVFSRC (the DRAM's and vcore's level) on the MT6897: what its registers say, pure. Layout from MediaTek's
 * request path for this SoC (MiCode MTK_kernel_device_modules, drivers/soc/mediatek/mtk-dvfsrc.c, mt6983 series):
 *   DVFSRC_LEVEL  0x5f0: bits 5:0 the current level, the opp index being DVFSRC_OPPS - (field + 1); bit 16 a change under way
 *   DVFSRC_SW_REQ 0x18:  bits 15:12 the DDR opp software asks for (0..9, 9 the fastest), bits 6:4 the vcore opp
 * Under Android the GPU's driver raises the DRAM so (trial 35: five times the GPU clock, not twice the speed).
 */
#ifndef HAKIOS_DVFSRC_H
#define HAKIOS_DVFSRC_H
#include <stdint.h>

#define DVFSRC_LEVEL      0x5f0u
#define DVFSRC_SW_REQ     0x18u
#define DVFSRC_OPPS       42      /* mt6897's lp5x and lp5 tables: 42 {vcore, dram} pairs */
#define DVFSRC_DDR_MAX    9       /* dram opp 0..9 */

/* the current opp index (0 the slowest, 41 the fastest), or -1 */
static inline int dvfsrc_opp(uint32_t level) {
	int field = (int)(level & 0x3fu) + 1;
	return field > DVFSRC_OPPS ? -1 : DVFSRC_OPPS - field;
}
static inline int dvfsrc_busy(uint32_t level) { return (level >> 16) & 1u; }
static inline uint32_t dvfsrc_with_ddr(uint32_t sw_req, int ddr) {
	if (ddr < 0 || ddr > DVFSRC_DDR_MAX) return sw_req;
	return (sw_req & ~(0xfu << 12)) | ((uint32_t)ddr << 12);
}
/* both registers read as DVFSRC's: a level of the 42, a DDR request of 0..9 (an unmapped block reads 0xffffffff) */
static inline int dvfsrc_sane(uint32_t level, uint32_t sw_req) {
	return dvfsrc_opp(level) >= 0 && ((sw_req >> 12) & 0xfu) <= DVFSRC_DDR_MAX && level != 0xffffffffu;
}
#endif
