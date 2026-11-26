/*
 * Copyright (c) 2026 HakiOS. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed software.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
/*
 * One reserved range inside the memory the loader gives XNU, which the kernel must never hand out
 * nor map (boot-args ram_hole=, ram_hole_size=). On the POCO X6 Pro it is mblock-8-dramc-rk0, 8 KiB at
 * 0x1bfffe000, between the 2 GiB below it and the 5.83 GiB above it. Pure: tested on the Mac.
 */
#ifndef _ARM64_HAKIOS_RAM_HOLE_H_
#define _ARM64_HAKIOS_RAM_HOLE_H_

#include <stdint.h>

struct hakios_ram_hole {
	uint64_t start;         /* the whole pages it touches, end exclusive; start == end is no hole */
	uint64_t end;
};

static inline struct hakios_ram_hole
hakios_ram_hole_make(uint64_t pa, uint64_t len, uint64_t page_mask)
{
	struct hakios_ram_hole h = { 0, 0 };

	if (len != 0) {
		h.start = pa & ~page_mask;
		h.end = (pa + len + page_mask) & ~page_mask;
	}
	return h;
}

static inline int
hakios_ram_hole_present(struct hakios_ram_hole h)
{
	return h.end > h.start;
}

/*
 * Whether pa is in the hole. The VM keeps the page's place in vm_pages[] (on arm64 a page's address is its index there) and
 * never frees it, as it does with bad RAM; the physical aperture leaves it unmapped.
 */
static inline int
hakios_ram_hole_contains(struct hakios_ram_hole h, uint64_t pa)
{
	return pa >= h.start && pa < h.end;
}

/* How many of the pages in [first, end) are in the hole. */
static inline uint64_t
hakios_ram_hole_pages_in(struct hakios_ram_hole h, uint64_t first, uint64_t end, uint64_t page_size)
{
	uint64_t lo = first > h.start ? first : h.start;
	uint64_t hi = end < h.end ? end : h.end;

	return hi > lo ? (hi - lo) / page_size : 0;
}

#ifdef KERNEL
extern struct hakios_ram_hole hakios_ram_hole;        /* arm_vm_init.c */
#endif

#endif /* _ARM64_HAKIOS_RAM_HOLE_H_ */
