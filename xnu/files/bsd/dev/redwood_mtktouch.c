/*
 * duchamp (POCO X6 Pro, MT6897): the touch screen, as a source of touches for /dev/redwood-touch
 * (sys/redwood_input.h). Only with boot-arg mtktouch=1 (a Goodix GT9916) or mtktouch=2 (a FocalTech FT3683G):
 * every address below belongs to this phone.
 *
 * The phone comes with either chip, on the same pins. mtkloader (XNU_TOUCH=1, mtkloader/mtktouch.h) powers it on
 * the way here -- the MT6368's VTP supply at 1.8 V, the analog supply's switch on GPIO73, the reset on GPIO60 --
 * asks which of the two answers and says so on the command line, with what the Goodix chip says of itself:
 *   mtktouch=N      1 Goodix, 2 FocalTech
 *   mtktouch_at=A   where the Goodix chip keeps its touch report (0x10308 on Isaac's phone)
 *   mtktouch_x=X, mtktouch_y=Y   the range of the coordinates (19520 x 43392: 1/16 of a pixel of the 1220 x 2712
 *                   panel, for both chips)
 * So all that is left here is to read what the chip says, the way hakios-mtk's TD4160 driver (redwood_tcm.c) reads
 * the chip its loader woke.
 *
 * There is no pin or SPI framework in this kernel, and the chip is read on the SPI2 pins driven by hand, as the
 * loader does: GPIO11 the clock, 12 the chip select, 13 data in, 14 data out, SPI mode 0. GPIO9 is the interrupt
 * line, but it says little: in the loader's probe on Isaac's phone (11 November) it was low in 614 of 5000 looks while
 * 3699 of them found a new report, and low 333 times in five seconds with no finger at all. So no interrupt and no
 * line: a thread reads the report every 8 ms (boot-arg mtktouch_poll_us=N), which costs one 34-byte transfer when
 * nothing is there (the chip's status byte is then 0). mtktouch_flip=1 mirrors left-right, 2 top-bottom, 3 both;
 * Android uses none (its tree has no flip for this node), and the probe saw x run 31-19515 and y 156-40732.
 *
 * The chips' languages, from the vendor drivers (github.com/mt6897-devs/android_kernel_device_modules_6.1,
 * drivers/input/touchscreen/goodix_9916 and focaltech_3683g):
 *   Goodix     read: 0xF1, the 32-bit address, three 0xFF, then the data; write: 0xF0, the address, the data.
 *              The report: an 8-byte head (status, -, finger count in the low four bits, ..., a 16-bit sum),
 *              8 bytes a finger (id in the high four bits of the first, x and y little-endian at 2 and 4), a
 *              16-bit sum; writing 0 to its first byte says it has been read.
 *   FocalTech  read: the register, 0xA0, the 16-bit length, three dummy bytes, then the data and its
 *              CRC-16/Kermit; the chip's status is the 4th byte back. The report at register 0x01, 84 bytes:
 *              protocol v2, 8 bytes a finger after a 4-byte head.
 */
#ifndef MTKTOUCH_HOST_TEST            /* defined only when built on a computer against simulated chips */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
#endif

#define GPIO_PHYS       0x10005000UL
#define GPIO_SIZE       0x1000UL

#define PIN_INT         9
#define PIN_CLK         11
#define PIN_CS          12
#define PIN_MI          13
#define PIN_MO          14
#define PIN_RESET       60

#define MAX_FINGERS     10
#define CHIP_GOODIX     1
#define CHIP_FOCALTECH  2

static vm_offset_t gpio;
static uint32_t chip, report_at, max_x = 19520, max_y = 43392, flip;

/* ---- pins: "race free" registers, +4 sets the bits written, +8 clears them ---- */

#ifndef MTKTOUCH_HOST_TEST
static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}
#endif

static inline void
pin_set(uint32_t pin, bool high)
{
	wr(gpio + 0x100 + 0x10 * (pin / 32) + (high ? 4 : 8), 1u << (pin % 32));
}

static inline uint32_t
pin_get(uint32_t pin)
{
	return (rd(gpio + 0x200 + 0x10 * (pin / 32)) >> (pin % 32)) & 1;
}

static inline void
pin_settle(void)
{
	(void)rd(gpio + 0x200);
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, NSEC_PER_MSEC);
}

/* ---- SPI by hand, mode 0: the clock idles low, both sides sample on its rising edge ---- */

static void
transfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
	pin_set(PIN_CLK, false);
	pin_set(PIN_CS, false);
	pin_settle();
	for (uint32_t i = 0; i < len; i++) {
		uint8_t out = tx ? tx[i] : 0, in = 0;

		for (int bit = 7; bit >= 0; bit--) {
			pin_set(PIN_MO, (out >> bit) & 1);
			pin_settle();
			pin_set(PIN_CLK, true);
			pin_settle();
			in = (uint8_t)(in << 1 | pin_get(PIN_MI));
			pin_set(PIN_CLK, false);
		}
		if (rx) {
			rx[i] = in;
		}
	}
	pin_settle();
	pin_set(PIN_CS, true);
	pin_settle();
}

static uint8_t tx_buf[8 + 128], rx_buf[8 + 128];

static uint32_t
le16(const uint8_t *p)
{
	return p[0] | (uint32_t)p[1] << 8;
}

/* ---- Goodix GT9916 ---- */

static void
goodix_read(uint32_t address, uint8_t *data, uint32_t length)
{
	tx_buf[0] = 0xf1;
	tx_buf[1] = (uint8_t)(address >> 24);
	tx_buf[2] = (uint8_t)(address >> 16);
	tx_buf[3] = (uint8_t)(address >> 8);
	tx_buf[4] = (uint8_t)address;
	tx_buf[5] = tx_buf[6] = tx_buf[7] = 0xff;
	memset(tx_buf + 8, 0, length);
	transfer(tx_buf, rx_buf, 8 + length);
	memcpy(data, rx_buf + 8, length);
}

static void
goodix_write(uint32_t address, const uint8_t *data, uint32_t length)
{
	tx_buf[0] = 0xf0;
	tx_buf[1] = (uint8_t)(address >> 24);
	tx_buf[2] = (uint8_t)(address >> 16);
	tx_buf[3] = (uint8_t)(address >> 8);
	tx_buf[4] = (uint8_t)address;
	memcpy(tx_buf + 5, data, length);
	transfer(tx_buf, NULL, 5 + length);
}

/* The chip's blocks end in the 16-bit sum of their bytes; all zeros or all ones is no answer at all. */
static bool
goodix_sum_ok(const uint8_t *data, uint32_t length)
{
	uint32_t sum = 0;
	bool blank = true;

	for (uint32_t i = 0; i + 2 < length; i++) {
		sum += data[i];
		blank &= data[i] == 0 || data[i] == 0xff;
	}
	return length > 2 && !blank && (sum & 0xffff) == le16(data + length - 2);
}

/* ---- FocalTech FT3683G ---- */

static uint16_t
focaltech_crc(const uint8_t *data, uint32_t length)
{
	uint32_t crc = 0xffff;

	for (uint32_t i = 0; i < length; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
		}
	}
	return (uint16_t)crc;
}

static bool
focaltech_read(uint8_t address, uint8_t *data, uint32_t length)
{
	const uint32_t at = 7, total = at + length + 2;

	if (total > sizeof(tx_buf)) {
		return false;
	}
	for (int attempt = 0; attempt < 3; attempt++) {
		memset(tx_buf, 0, total);
		tx_buf[0] = address;
		tx_buf[1] = 0xa0;                       /* read, with a CRC */
		tx_buf[2] = (uint8_t)(length >> 8);
		tx_buf[3] = (uint8_t)length;
		transfer(tx_buf, rx_buf, total);
		sleep_us(150);                          /* the chip select's least time high (the vendor's CS_HIGH_DELAY) */
		if ((rx_buf[3] & 0xa0) == 0 && focaltech_crc(rx_buf + at, length) == le16(rx_buf + at + length)) {
			memcpy(data, rx_buf + at, length);
			return true;
		}
	}
	return false;
}

/* ---- fingers ---- */

static struct {
	uint32_t looks, reports, samples, touches, bad, requests, last_x, last_y;
} seen;

static struct {
	bool down, seen;
	uint32_t x, y;
} finger[MAX_FINGERS];

/* A finger in this report, in the chip's coordinates. */
static void
finger_at(uint32_t id, uint32_t x, uint32_t y)
{
	static unsigned shown;

	if (id >= MAX_FINGERS) {
		return;
	}
	seen.samples++;
	seen.last_x = x;
	seen.last_y = y;
	x = x >= max_x ? 65535 : x * 65535 / max_x;
	y = y >= max_y ? 65535 : y * 65535 / max_y;
	if (flip & 1) {
		x = 65535 - x;
	}
	if (flip & 2) {
		y = 65535 - y;
	}
	finger[id].seen = true;
	if (!finger[id].down) {
		seen.touches++;
		if (shown < 20) {
			shown++;
			printf("mtktouch: finger %u down at %u, %u of 65535\n", id, x, y);
		}
		redwood_touch_post(REDWOOD_TOUCH_DOWN, id, x, y);
	} else if (finger[id].x != x || finger[id].y != y) {
		redwood_touch_post(REDWOOD_TOUCH_MOVE, id, x, y);
	}
	finger[id].down = true;
	finger[id].x = x;
	finger[id].y = y;
}

/* The report has been taken in: a finger that was down and is not in it has lifted. */
static void
report_done(void)
{
	for (uint32_t id = 0; id < MAX_FINGERS; id++) {
		if (finger[id].down && !finger[id].seen) {
			finger[id].down = false;
			redwood_touch_post(REDWOOD_TOUCH_UP, id, finger[id].x, finger[id].y);
		}
		finger[id].seen = false;
	}
}

static void
goodix_poll(void)
{
	static uint8_t report[8 + 8 * MAX_FINGERS + 2];
	static unsigned shown;
	const uint8_t zero = 0;
	uint32_t count;

	seen.looks++;
	goodix_read(report_at, report, 8 + 8 * 2 + 2);  /* the head and two fingers: all there is, most of the time */
	if (report[0] == 0) {
		return;
	}
	seen.reports++;
	count = report[2] & 0xf;
	if ((report[0] & 0x80) && count > 2 && count <= MAX_FINGERS) {
		goodix_read(report_at + 26, report + 26, 8 * (count - 2));
	}
	goodix_write(report_at, &zero, 1);
	if (!goodix_sum_ok(report, 8)) {
		if (seen.bad++ < 8) {
			printf("mtktouch: bad report head %02x %02x %02x %02x %02x %02x %02x %02x\n", report[0], report[1],
			    report[2], report[3], report[4], report[5], report[6], report[7]);
		}
		return;
	}
	if (report[0] & 0x40) {
		/* a request; 3 is "reset me" */
		seen.requests++;
		if (shown++ < 8) {
			printf("mtktouch: the chip asks for %#x\n", report[2]);
		}
		if (report[2] == 3 || (report[2] & 0xf) == 3) {
			pin_set(PIN_RESET, false);
			sleep_ms(2);
			pin_set(PIN_RESET, true);
			sleep_ms(100);
		}
		return;
	}
	if (!(report[0] & 0x80)) {
		return;
	}
	if (count > MAX_FINGERS || (count && !goodix_sum_ok(report + 8, 8 * count + 2))) {
		if (seen.bad++ < 8) {
			printf("mtktouch: bad report (%u fingers)\n", count);
		}
		return;
	}
	for (uint32_t i = 0; i < count; i++) {
		const uint8_t *t = report + 8 + 8 * i;
		uint32_t kind = t[0] & 0xf;

		if (kind != 1 && kind != 3) {          /* 1 and 3: a stylus */
			finger_at(t[0] >> 4, le16(t + 2), le16(t + 4));
		}
	}
	report_done();
}

static void
focaltech_poll(void)
{
	static uint8_t report[84];
	static unsigned shown;
	uint32_t type, count;

	seen.looks++;
	if (!focaltech_read(0x01, report, sizeof(report))) {
		seen.bad++;
		return;
	}
	if (report[1] == 0xff && report[2] == 0xff && report[3] == 0xff && report[4] == 0xff) {
		return;                                 /* the firmware has only just started */
	}
	seen.reports++;
	type = report[1] >> 4;
	count = report[1] & 0xf;
	if (type == 2) {
		for (uint32_t i = 0; i < count && i < MAX_FINGERS; i++) {
			const uint8_t *t = report + 4 + 8 * i;

			if ((t[0] >> 6) != 1) {         /* 0 down, 2 still down; 1 is a finger lifting */
				finger_at(t[2] >> 4, (uint32_t)(t[0] & 15) << 12 | (uint32_t)t[1] << 4 | t[4] >> 4,
				    (uint32_t)(t[2] & 15) << 12 | (uint32_t)t[3] << 4 | (t[4] & 15u));
			}
		}
	} else if (type == 0) {
		for (uint32_t i = 0; i < MAX_FINGERS; i++) {
			const uint8_t *t = report + 2 + 6 * i;

			if ((t[2] >> 4) >= MAX_FINGERS) {
				break;
			}
			if ((t[0] >> 6) != 1) {
				finger_at(t[2] >> 4, (uint32_t)(t[0] & 15) << 12 | (uint32_t)t[1] << 4 | t[4] >> 4,
				    (uint32_t)(t[2] & 15) << 12 | (uint32_t)t[3] << 4 | (t[4] & 15u));
			}
		}
	} else if (shown++ < 8) {
		printf("mtktouch: report of type %u: %02x %02x %02x %02x\n", type, report[0], report[1], report[2], report[3]);
	}
	report_done();
}

/* ---- the thread ---- */

static void
mtktouch_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t poll_us = 8000;
	const char *name = chip == CHIP_GOODIX ? "Goodix GT9916" : "FocalTech FT3683G";

	PE_parse_boot_argn("mtktouch_poll_us", &poll_us, sizeof(poll_us));
	if (poll_us < 1000 || poll_us > 100000) {
		poll_us = 8000;
	}
	gpio = ml_io_map(GPIO_PHYS, GPIO_SIZE);
	if (gpio == 0) {
		printf("mtktouch: cannot map the pins\n");
		thread_terminate(current_thread());
		return;
	}
	if (chip == CHIP_GOODIX && report_at == 0) {
		printf("mtktouch: no report address (mtktouch_at=): no touch screen\n");
		thread_terminate(current_thread());
		return;
	}
	printf("mtktouch: %s, report at %#x, coordinates up to %u x %u, flip %u; read every %u us\n", name,
	    report_at, max_x, max_y, flip, poll_us);
	for (uint32_t tick = 1;; tick++) {
		if (chip == CHIP_FOCALTECH) {
			focaltech_poll();
		} else {
			goodix_poll();
		}
		if (tick % (60 * (1000000 / poll_us)) == 0) {
			printf("mtktouch: so far %u reports in %u reads (%u bad, %u requests), %u touches, %u finger positions, "
			    "the last at %u, %u; the line %s\n", seen.reports, seen.looks, seen.bad, seen.requests, seen.touches,
			    seen.samples, seen.last_x, seen.last_y, pin_get(PIN_INT) ? "high" : "low");
		}
		sleep_us(poll_us);
	}
}

void
redwood_mtktouch_init(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("mtktouch", &chip, sizeof(chip)) || (chip != CHIP_GOODIX && chip != CHIP_FOCALTECH)) {
		return;
	}
	PE_parse_boot_argn("mtktouch_at", &report_at, sizeof(report_at));
	PE_parse_boot_argn("mtktouch_x", &max_x, sizeof(max_x));
	PE_parse_boot_argn("mtktouch_y", &max_y, sizeof(max_y));
	PE_parse_boot_argn("mtktouch_flip", &flip, sizeof(flip));
	if (max_x == 0 || max_y == 0) {
		max_x = 19520;
		max_y = 43392;
	}
	if (kernel_thread_start(mtktouch_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("mtktouch: cannot start the thread that reads the touch screen\n");
	}
}
