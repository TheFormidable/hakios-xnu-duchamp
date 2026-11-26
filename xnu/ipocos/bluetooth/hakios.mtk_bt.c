/* SPDX-License-Identifier: MIT
 * Duchamp MT6897 BGFSYS and BEIF H4 transport.
 *
 * Register order cross-checked against the handset's bt_drv_6897 and
 * mt6897-devs' platform_6897.h. The BT firmware is preloaded by LK.
 */
#include <IOKit/IOBSD.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/kauth.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include "hakios.mtk_bt_beif.h"

extern vm_offset_t ml_io_map_unmappable(vm_offset_t, vm_size_t, unsigned);
extern void ml_io_unmap(vm_offset_t, vm_size_t);
extern void IOSleep(unsigned);
extern void delay(int);
extern void loader_log_line(const char *);
extern int hakios_mtk_conn_bt_status(bool);
extern bool hakios_mtk_bt_publish_address(const uint8_t *);
extern bool hakios_mtk_bt_publish_ready(void);
void hakios_mtk_bt_attach(void);

#define BT_BIT(n) (1u << (n))
#define BT_EMI 0x87e00000u
#define BT_WAKE 0x180601a8u
#define BT_RGU_POWER 0x18000020u
#define BT_MCU_RESET 0x18000124u
#define BT_FUNC_ENABLE 0x18001208u
#define BT_CONN2BT 0x18001450u
#define BT_BT2CONN 0x18001460u
#define BT_SW_IRQ 0x1803f014u
#define BT_DOORBELL 0x1803f00cu
#define BT_OWNERSHIP 0x18060030u
#define BT_OWNERSHIP_IRQ 0x18060034u
#define BT_EMI_REQUEST 0x18011118u
#define BT_TOP_CLOCK 0x18003128u
#define BT_SEMA_RESET 0x1800015cu
#define BT_MCU_PC 0x18023a00u
#define BT_SW_IRQ_STATUS 0x1803f010u

static LCK_GRP_DECLARE(bt_lock_group, "mt6897-bt");
static LCK_MTX_DECLARE(bt_lock, &bt_lock_group);
static struct selinfo bt_read_select, bt_write_select;
static struct termios bt_serial;
static volatile uint8_t *bt_beif;
static bool bt_opened, bt_powered, bt_reported, bt_broken;
static uintptr_t bt_generation;
static kern_ctl_ref bt_control;
static uint32_t bt_control_unit;
static unsigned bt_log_commands;
static struct bt_h4_observer bt_tx_observer, bt_rx_observer;
/* Whole-boot cap, deliberately not reset when bluetoothd reconnects. */
static unsigned bt_trace_budget = 128;

SYSCTL_DECL(_kern_hakios);

static int
bt_ready_sysctl SYSCTL_HANDLER_ARGS
{
	lck_mtx_lock(&bt_lock);
	int ready = bt_control && bt_powered && !bt_broken;
	lck_mtx_unlock(&bt_lock);
	return SYSCTL_OUT(req, &ready, sizeof(ready));
}

SYSCTL_PROC(_kern_hakios, OID_AUTO, bt_ready, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED,
    NULL, 0, bt_ready_sysctl, "I", "MediaTek Bluetooth firmware is running");

/* These are Device mappings. The pages are mapped before their first access;
 * BGFSYS is only read after both power acknowledgements and bus gates agree. */
static struct {
	uint32_t base, size;
	volatile uint8_t *map;
} bt_windows[] = {
	{ 0x18000000, 0x4000, NULL }, { 0x18010000, 0x4000, NULL },
	{ 0x18020000, 0x4000, NULL }, { 0x1803c000, 0x4000, NULL },
	{ 0x18060000, 0x4000, NULL }, { 0x18070000, 0x4000, NULL },
	{ 0x18800000, 0x4000, NULL },
	{ 0x18810000, 0x4000, NULL }, { 0x18820000, 0x4000, NULL },
	{ BT_EMI + 0xb48000, 0x4000, NULL },
	{ BT_EMI + 0xba0000, BT_BEIF_BYTES, NULL },
};

static void
bt_note(const char *step, uint32_t value)
{
	char line[160];

	snprintf(line, sizeof(line), "mtkbt: %s [%#x]\n", step, value);
	loader_log_line(line);
}

static void
bt_trace_packet(const uint8_t *p, unsigned length, void *context)
{
    unsigned opcode = 0, status = 0;
    bool tx = context != NULL;
    if (!bt_trace_budget)
        return;
    if (tx && p[0] == 1 && length >= 4) {
        opcode = p[1] | ((unsigned)p[2] << 8);
        if (opcode != 0x200b && opcode != 0x200c && opcode != 0x2041 &&
            opcode != 0x2042 && opcode != 0x2006 && opcode != 0x2036 && opcode != 0x0c03)
            return;
        if (length > 4)
            status = p[4]; /* scan/advertising mode, never an address */
    } else if (!tx && p[0] == 4 && p[1] == 0x0e && length >= 7) {
        opcode = p[4] | ((unsigned)p[5] << 8);
        status = p[6];
    } else if (!tx && p[0] == 4 && p[1] == 0x0f && length >= 7) {
        opcode = p[5] | ((unsigned)p[6] << 8);
        status = p[3];
    } else {
        return;
    }
    bt_trace_budget--;
    bt_note(tx ? "HCI TX opcode/mode" : "HCI RX opcode/status", opcode | (status << 16));
}

static volatile uint32_t *
bt_register(uint32_t address)
{
	for (unsigned i = 0; i < sizeof(bt_windows) / sizeof(bt_windows[0]); i++) {
		if (address >= bt_windows[i].base && address - bt_windows[i].base < bt_windows[i].size)
			return (volatile uint32_t *)(bt_windows[i].map + (address - bt_windows[i].base));
	}
	panic("mtkbt: address %#x is outside the mapped registers", address);
}

static uint32_t
bt_reg_read(uint32_t address)
{
	return *bt_register(address);
}

static void
bt_reg_write(uint32_t address, uint32_t value)
{
	*bt_register(address) = value;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static void
bt_reg_update(uint32_t address, uint32_t mask, uint32_t bits)
{
	bt_reg_write(address, (bt_reg_read(address) & ~mask) | (bits & mask));
}

static bool
bt_wait(uint32_t address, uint32_t mask, uint32_t expected, unsigned iterations, const char *step)
{
	uint32_t value = 0;

	for (unsigned i = 0; i < iterations; i++) {
		value = bt_reg_read(address);
		if ((value & mask) == expected)
			return true;
		IOSleep(1);
	}
	bt_note(step, value);
	return false;
}

static bool
bt_gate(uint32_t address, unsigned enable_bit, unsigned ack_bit, bool protect)
{
	const uint32_t enable = BT_BIT(enable_bit), ack = BT_BIT(ack_bit);

	bt_reg_update(address, enable, protect ? enable : 0);
	return bt_wait(address + 4, ack, protect ? ack : 0, 50, "Bluetooth bus gate timeout");
}

static void
bt_unmap(void)
{
	for (unsigned i = 0; i < sizeof(bt_windows) / sizeof(bt_windows[0]); i++) {
		if (bt_windows[i].map) {
			ml_io_unmap((vm_offset_t)bt_windows[i].map, bt_windows[i].size);
			bt_windows[i].map = NULL;
		}
	}
	bt_beif = NULL;
}

static bool
bt_map(void)
{
	for (unsigned i = 0; i < sizeof(bt_windows) / sizeof(bt_windows[0]); i++) {
		bt_windows[i].map = (volatile uint8_t *)ml_io_map_unmappable(bt_windows[i].base, bt_windows[i].size, 0);
		if (!bt_windows[i].map) {
			bt_note("cannot map region", bt_windows[i].base);
			bt_unmap();
			return false;
		}
	}
	bt_beif = (volatile uint8_t *)bt_register(BT_EMI + 0xba0000);
	return true;
}

static bool
bt_connsys_awake(void)
{
	bt_reg_update(BT_WAKE, BT_BIT(0), BT_BIT(0));
	delay(200);
	return bt_wait(0x18011000, UINT32_MAX, 0x02050400, 25, "connsys version timeout") &&
	       bt_wait(0x18001210, BT_BIT(16), BT_BIT(16), 25, "connsys restore timeout");
}

static void
bt_release_resources(void)
{
	bt_reg_update(BT_TOP_CLOCK, BT_BIT(0), 0);
	if (!bt_wait(BT_TOP_CLOCK, BT_BIT(1), 0, 30, "BT top clock release timeout"))
		bt_broken = true;
	for (uint32_t addr = 0x18071200; addr <= 0x18071260; addr += 4)
		bt_reg_write(addr, 1);
	bt_reg_update(BT_SEMA_RESET, BT_BIT(0), 0);
	bt_reg_update(BT_SEMA_RESET, BT_BIT(0), BT_BIT(0));
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(0), BT_BIT(0));
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(0), 0);
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(1), BT_BIT(1));
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(1) | BT_BIT(2) | BT_BIT(3), 0);
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(5), BT_BIT(5));
	bt_reg_update(BT_EMI_REQUEST, BT_BIT(5), 0);
}

static void
bt_down(void)
{
	if (bt_powered) {
		bool safe = bt_connsys_awake();
		if (safe) {
			/* Restore isolation before removing power. Try every gate. */
			safe = bt_gate(BT_BT2CONN, 0, 23, true);
			safe = bt_gate(BT_BT2CONN, 4, 22, true) && safe;
			safe = bt_gate(BT_CONN2BT, 0, 23, true) && safe;
			safe = bt_gate(BT_CONN2BT, 4, 22, true) && safe;
		}
		if (safe) {
			bt_reg_update(BT_FUNC_ENABLE, BT_BIT(0), 0);
			bt_reg_write(BT_RGU_POWER, 0x42540000 | (bt_reg_read(BT_RGU_POWER) & 0x0000ff7f));
			bt_reg_update(BT_MCU_RESET, BT_BIT(0), 0);
			IOSleep(1);
			bt_release_resources();
			bt_powered = false;
			bt_note("BGFSYS off", 0);
		} else {
			bt_broken = true;
			bt_note("BGFSYS isolation incomplete, retry requires reboot", 0);
		}
	}
	if (bt_reported && !bt_powered) {
		if (hakios_mtk_conn_bt_status(false) == 0)
			bt_reported = false;
		else
			bt_broken = true;
	}
	bt_reg_update(BT_WAKE, BT_BIT(0), 0);
}

static bool
bt_up(void)
{
	if (!bt_connsys_awake())
		return false;
	bt_note("connsys ready", 0x02050400);

	if (hakios_mtk_conn_bt_status(true) != 0) {
		bt_note("firmware rejected Bluetooth subsystem status", 0);
		return false;
	}
	bt_reported = true;
	bt_note("LK firmware version offset", bt_reg_read(BT_EMI + 0xb48024));
	bt_reg_update(BT_MCU_RESET, BT_BIT(0), 0);
	bt_beif_init(bt_beif);
	bt_reg_write(BT_RGU_POWER, 0x42540080 | (bt_reg_read(BT_RGU_POWER) & 0x0000ff7f));
	bt_powered = true;
	if (!bt_wait(0x18060a10, 3u << 18, 1u << 18, 50, "BGFSYS power isolation timeout"))
		return false;
	bt_reg_update(BT_FUNC_ENABLE, BT_BIT(0), BT_BIT(0));
	const uint32_t ack = BT_BIT(24) | BT_BIT(0);
	if (!bt_wait(0x18000424, ack, ack, 60, "BGFSYS power acknowledge timeout"))
		return false;
	if (!bt_gate(BT_CONN2BT, 4, 22, false) || !bt_gate(BT_CONN2BT, 0, 23, false) ||
	    !bt_gate(BT_BT2CONN, 4, 22, false) || !bt_gate(BT_BT2CONN, 0, 23, false))
		return false;
	delay(400);

	uint32_t version = bt_reg_read(0x18812000);
	bt_note("BGFSYS IP version", version);
	if (version != 0x02080000)
		return false;

	bt_reg_update(0x18800100, BT_BIT(31), 0);
	bt_reg_write(0x18800104, BT_BIT(2));
	bt_reg_update(0x18820284, 15u << 16, 8u << 16);
	bt_reg_update(0x18812100, BT_BIT(2), BT_BIT(2));
	uint32_t debug = bt_reg_read(0x18060c04);
	bt_reg_write(0x18060c04, (debug & 0xffc00000) | 0x300001);
	bool restored = bt_wait(0x18060c00, BT_BIT(15), BT_BIT(15), 60, "BGFSYS firmware restore timeout");
	if (restored) {
		bt_reg_write(0x18060034, BT_BIT(0));
		bt_reg_write(0x1806003c, BT_BIT(0));
		bt_reg_update(BT_MCU_RESET, BT_BIT(0), BT_BIT(0));
	}
	bt_reg_write(0x18060c04, debug);
	if (!restored)
		return false;

	bt_reg_write(BT_MCU_PC + 4, 0xc0010d2a);
	if (!bt_wait(0x1880016c, UINT32_MAX, 0x1d1e, 550, "BT ROM idle timeout")) {
		bt_note("MCU last PC", bt_reg_read(BT_MCU_PC));
		return false;
	}
	bt_note("BT ROM ready, MCU PC", bt_reg_read(BT_MCU_PC));
	bt_reg_write(BT_SW_IRQ, BT_BIT(0) | BT_BIT(1));

	bt_beif_init(bt_beif);
	if (!bt_beif_valid(bt_beif))
		return false;
	bt_note("BEIF rings ready", BT_RING_BYTES);
	bt_reg_update(BT_WAKE, BT_BIT(0), 0);
	return true;
}

static int
bt_send(const uint8_t *data, unsigned length)
{
	int sent = bt_beif_send(bt_beif, data, length);

	if (sent > 0)
		bt_reg_write(BT_DOORBELL, BT_BIT(0) | BT_BIT(1));
	return sent;
}

static bool
bt_claim_host(void)
{
	/* WMT and HCI are sent only after the controller leaves firmware own. */
	for (unsigned attempt = 0; attempt < 100; attempt++) {
		if (attempt % 8 == 0)
			bt_reg_write(BT_OWNERSHIP, BT_BIT(1));
		uint32_t state = bt_reg_read(BT_OWNERSHIP);
		if (!(state & BT_BIT(2))) {
			bt_reg_write(BT_OWNERSHIP_IRQ, BT_BIT(0));
			bt_note("host owns Bluetooth", state);
			return true;
		}
		IOSleep(1);
	}
	bt_note("Bluetooth ownership timeout", bt_reg_read(BT_OWNERSHIP));
	return false;
}

static bool
bt_command(const uint8_t *command, unsigned length, const uint8_t *reply, unsigned reply_length,
    uint8_t *data, unsigned data_length, const char *step)
{
	uint8_t event[260];
	unsigned count = 0;

	if (bt_send(command, length) != (int)length) {
		bt_note("could not submit initialization command", command[1]);
		return false;
	}
	for (unsigned i = 0; i < 1200; i++) {
		int got = bt_beif_receive(bt_beif, event + count, sizeof(event) - count);
		if (got < 0)
			break;
		count += (unsigned)got;
		while (count) {
			int packet_size = bt_h4_size(event, count);
			if (packet_size < 0 || packet_size > (int)sizeof(event))
				return false;
			if (!packet_size || (unsigned)packet_size > count)
				break;
			if ((unsigned)packet_size == reply_length + 1 + data_length && !memcmp(event, reply, reply_length)) {
				bt_note(step, event[reply_length]);
				if (data && !event[reply_length])
					memcpy(data, event + reply_length + 1, data_length);
				return event[reply_length] == 0;
			}
			count -= (unsigned)packet_size;
			memmove(event, event + packet_size, count);
		}
		IOSleep(1);
	}
	bt_note("initialization command timeout, opcode", command[1] | ((uint32_t)command[2] << 8));
	bt_note("bytes pending after timeout", count);
	const volatile uint32_t *words = (const volatile uint32_t *)bt_beif;
	bt_note("BEIF host TX head", words[BT_HOST_TX / 4]);
	bt_note("BEIF firmware RX tail", words[BT_FW_RX / 4]);
	bt_note("BEIF firmware TX head", words[BT_FW_TX / 4]);
	bt_note("BEIF host RX tail", words[BT_HOST_RX / 4]);
	bt_note("BGFSYS ownership after WMT", bt_reg_read(BT_OWNERSHIP));
	bt_note("BGFSYS MCU PC after WMT", bt_reg_read(BT_MCU_PC));
	bt_note("BGFSYS SW IRQ after WMT", bt_reg_read(BT_SW_IRQ_STATUS));
	return false;
}

static bool
bt_wmt_power(bool on)
{
	const uint8_t command[] = { 1, 0x6f, 0xfc, 6, 1, 6, 2, 0, 0, on ? 1 : 0 };
	const uint8_t reply[] = { 4, 0xe4, 5, 2, 6, 1, 0 };
	return bt_command(command, sizeof(command), reply, sizeof(reply), NULL, 0,
	           on ? "WMT function-on status" : "WMT function-off status");
}

static bool
bt_reset_hci(void)
{
	const uint8_t command[] = { 1, 3, 0x0c, 0 };
	const uint8_t reply[] = { 4, 0x0e, 4, 1, 3, 0x0c };
	return bt_command(command, sizeof(command), reply, sizeof(reply), NULL, 0, "HCI session reset status");
}

static bool
bt_read_address(uint8_t *address)
{
	const uint8_t command[] = { 1, 9, 0x10, 0 };
	const uint8_t reply[] = { 4, 0x0e, 10, 1, 9, 0x10 };
	uint8_t wire[6], any = 0, all = 0xff;

	if (!bt_command(command, sizeof(command), reply, sizeof(reply), wire, sizeof(wire), "HCI address status"))
		return false;
	for (unsigned i = 0; i < sizeof(wire); i++) {
		address[i] = wire[sizeof(wire) - 1 - i];
		any |= wire[i];
		all &= wire[i];
	}
	return any && all != 0xff;
}

static int
bt_rx_available(void)
{
	const volatile uint32_t *words = (const volatile uint32_t *)bt_beif;
	return bt_ring_used(words[BT_FW_TX / 4], words[BT_HOST_RX / 4]);
}

static int
bt_tx_room(void)
{
	const volatile uint32_t *words = (const volatile uint32_t *)bt_beif;
	int used = bt_ring_used(words[BT_HOST_TX / 4], words[BT_FW_RX / 4]);
	return used < 0 ? -1 : BT_RING_BYTES - 1 - used;
}

static void
bt_notify_thread(void *arg, __unused wait_result_t result)
{
	const uintptr_t generation = (uintptr_t)arg;
	uint8_t pending[2048];
	unsigned pending_count = 0;
	int previous_room = -1;

	for (;;) {
		lck_mtx_lock(&bt_lock);
		if (!bt_opened || bt_generation != generation) {
			lck_mtx_unlock(&bt_lock);
			return;
		}
		int available = bt_rx_available(), room = bt_tx_room();
		if (bt_control_unit) {
			if (!pending_count && available > 0) {
				int received = bt_beif_receive(bt_beif, pending, MIN(available, (int)sizeof(pending)));
				if (received > 0) {
                    if (bt_trace_budget)
                        bt_h4_observe(&bt_rx_observer, pending, (unsigned)received, bt_trace_packet, NULL);
                    pending_count = (unsigned)received;
                }
			}
			if (pending_count && !ctl_enqueuedata(bt_control, bt_control_unit, pending, pending_count, 0))
				pending_count = 0;
		} else if (available != 0) {
			wakeup(&bt_read_select);
			selwakeup(&bt_read_select);
		}
		if (room != previous_room) {
			previous_room = room;
			wakeup(&bt_write_select);
			selwakeup(&bt_write_select);
		}
		lck_mtx_unlock(&bt_lock);
		IOSleep(available > 0 && !pending_count ? 2 : 8);
	}
}

static int
bt_prepare(void)
{
	if (bt_broken || (!bt_powered && !bt_map()))
		return EIO;
	bool resume = bt_powered;
	bool ready = resume ? bt_connsys_awake() : bt_up();
	if (!ready || !bt_claim_host() || (!resume && !bt_wmt_power(true))) {
		bt_down();
		bt_unmap();
		bt_broken = true;
		return EIO;
	}
	bt_reg_update(BT_WAKE, BT_BIT(0), 0);
	return 0;
}

static int
bt_start(uint32_t unit)
{
	thread_t worker;
	int error = 0;

	lck_mtx_lock(&bt_lock);
	if (bt_opened) {
		error = EBUSY;
		goto done;
	}
	error = bt_prepare();
	if (error)
		goto done;
	bt_opened = true;
	bt_control_unit = unit;
	bt_log_commands = 0;
    bzero(&bt_tx_observer, sizeof(bt_tx_observer));
    bzero(&bt_rx_observer, sizeof(bt_rx_observer));
	if (kernel_thread_start(bt_notify_thread, (void *)++bt_generation, &worker) != KERN_SUCCESS) {
		bt_opened = false;
		bt_control_unit = 0;
		(void)bt_wmt_power(false);
		bt_down();
		bt_unmap();
		error = EAGAIN;
		goto done;
	}
	thread_deallocate(worker);
	bt_note("H4 transport opened, control unit", unit);
done:
	lck_mtx_unlock(&bt_lock);
	return error;
}

static void
bt_stop(uint32_t unit)
{
	lck_mtx_lock(&bt_lock);
	if (bt_opened && bt_control_unit == unit) {
		bt_opened = false;
		bt_control_unit = 0;
		wakeup(&bt_read_select);
		wakeup(&bt_write_select);
		selwakeup(&bt_read_select);
		selwakeup(&bt_write_select);
		/* End discovery and connections. Keep MCU ownership until the sleep
		 * wakeup path is reliable with concurrent Wi-Fi. */
		bool awake = bt_connsys_awake() && bt_claim_host();
		if (!awake || !bt_reset_hci()) {
			if (awake)
				(void)bt_wmt_power(false);
			bt_down();
			bt_unmap();
			bt_broken = true;
		} else
			bt_reg_update(BT_WAKE, BT_BIT(0), 0);
	}
	lck_mtx_unlock(&bt_lock);
}

static int
bt_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return bt_start(0);
}

static int
bt_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	bt_stop(0);
	return 0;
}

static int
bt_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t bytes[2048];
	int error = 0;

	if (!uio_resid(uio))
		return 0;
	lck_mtx_lock(&bt_lock);
	while (bt_opened && bt_rx_available() == 0) {
		if (ioflag & IO_NDELAY) {
			error = EWOULDBLOCK;
			break;
		}
		error = msleep(&bt_read_select, &bt_lock, PZERO | PCATCH, "bt-read", NULL);
		if (error)
			break;
	}
	if (!error) {
		unsigned amount = (unsigned)MIN(uio_resid(uio), sizeof(bytes));
		int got = bt_opened ? bt_beif_receive(bt_beif, bytes, amount) : -1;
		error = got < 0 ? EIO : uiomove((char *)bytes, got, uio);
	}
	lck_mtx_unlock(&bt_lock);
	return error;
}

static int
bt_write(__unused dev_t dev, struct uio *uio, int ioflag)
{
	uint8_t bytes[2048];
	int error = 0;

	lck_mtx_lock(&bt_lock);
	while (uio_resid(uio) > 0 && !error) {
		unsigned amount = (unsigned)MIN(uio_resid(uio), sizeof(bytes));
		int room = bt_opened ? bt_tx_room() : -1;

		if (room < 0) {
			error = EIO;
			break;
		}
		if ((unsigned)room < amount) {
			if (ioflag & IO_NDELAY) {
				error = EWOULDBLOCK;
				break;
			}
			error = msleep(&bt_write_select, &bt_lock, PZERO | PCATCH, "bt-write", NULL);
			continue;
		}
		error = uiomove((char *)bytes, amount, uio);
		if (!error && bt_send(bytes, amount) != (int)amount)
			error = EIO;
		if (!error && amount >= 4 && bytes[0] == 1 && bt_log_commands++ < 20)
			bt_note("HCI command", bytes[1] | ((uint32_t)bytes[2] << 8));
	}
	lck_mtx_unlock(&bt_lock);
	return error;
}

static int
bt_select(__unused dev_t dev, int which, void *waiter, struct proc *p)
{
	lck_mtx_lock(&bt_lock);
	int ready = !bt_opened || (which == FREAD ? bt_rx_available() != 0 : bt_tx_room() != 0);
	if (!ready)
		selrecord(p, which == FREAD ? &bt_read_select : &bt_write_select, waiter);
	lck_mtx_unlock(&bt_lock);
	return ready;
}

static int
bt_ioctl(__unused dev_t dev, u_long command, caddr_t data, __unused int flags, __unused struct proc *p)
{
	int error = 0;

	lck_mtx_lock(&bt_lock);
	switch (command) {
	case TIOCGETA:
		*(struct termios *)data = bt_serial;
		break;
	case TIOCSETA: case TIOCSETAW: case TIOCSETAF:
		bt_serial = *(const struct termios *)data;
		break;
	case FIONREAD:
		*(int *)data = bt_opened ? MAX(0, bt_rx_available()) : 0;
		break;
	case TIOCMGET:
		*(int *)data = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
		break;
	case FIONBIO: case TIOCEXCL: case TIOCNXCL: case TIOCFLUSH:
	case TIOCMSET: case TIOCMBIS: case TIOCMBIC:
	case TIOCSDTR: case TIOCCDTR:
		/* H4 sees a serial byte stream. BEIF has no modem wires. */
		break;
	default:
		error = ENOTTY;
		break;
	}
	lck_mtx_unlock(&bt_lock);
	return error;
}

static const struct cdevsw bt_switch = {
	.d_open = bt_open, .d_close = bt_close, .d_read = bt_read, .d_write = bt_write,
	.d_ioctl = bt_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = bt_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat,
};

/* The stock daemon runs as mobile and owns the Bluetooth entitlement. */
static errno_t
bt_control_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *address, void **context)
{
	if (!address->sc_unit)
		return EINVAL;
	if (!kauth_cred_issuser(kauth_cred_get()) &&
	    !IOCurrentTaskHasEntitlement("com.apple.bluetooth.internal"))
		return EPERM;
	int error = bt_start(address->sc_unit);
	*context = error ? NULL : &bt_control;
	return error;
}

static errno_t
bt_control_disconnect(__unused kern_ctl_ref ref, uint32_t unit, void *context)
{
	if (context == &bt_control)
		bt_stop(unit);
	return 0;
}

static errno_t
bt_control_send(__unused kern_ctl_ref ref, uint32_t unit, void *context, mbuf_t mbuf, int flags)
{
	size_t length = mbuf_pkthdr_len(mbuf);
	int error = 0;
	uint8_t *bytes = NULL;

	if (context != &bt_control)
		error = ENOTCONN;
	else if (flags & MSG_OOB)
		error = EOPNOTSUPP;
	else if (length >= BT_RING_BYTES)
		error = EMSGSIZE;
	else if (length) {
		bytes = kalloc_data(length, Z_WAITOK);
		error = bytes ? mbuf_copydata(mbuf, 0, length, bytes) : ENOMEM;
	}
	mbuf_freem(mbuf);
	if (error || !length)
		goto done;

	lck_mtx_lock(&bt_lock);
	for (unsigned i = 0; ; i++) {
		if (!bt_opened || bt_control_unit != unit) {
			error = ENOTCONN;
			break;
		}
		int sent = bt_send(bytes, (unsigned)length);
		if (sent == (int)length) {
            if (bt_trace_budget)
                bt_h4_observe(&bt_tx_observer, bytes, (unsigned)length, bt_trace_packet, (void *)1);
			if (length >= 4 && bytes[0] == 1 && bt_log_commands++ < 20)
				bt_note("native HCI command", bytes[1] | ((uint32_t)bytes[2] << 8));
			break;
		}
		if (sent < 0 || i == 8) {
			error = sent < 0 ? EIO : ENOBUFS;
			break;
		}
		const struct timespec wait = { .tv_nsec = 8 * NSEC_PER_MSEC };
		error = msleep(&bt_write_select, &bt_lock, PZERO | PCATCH, "bt-ctl-write", &wait);
		if (error && error != EWOULDBLOCK)
			break;
		error = 0;
	}
	lck_mtx_unlock(&bt_lock);
done:
	if (bytes)
		kfree_data(bytes, length);
	return error;
}

static errno_t
bt_control_get(__unused kern_ctl_ref ref, __unused uint32_t unit, void *context,
    int option, void *data, size_t *length)
{
	if (context != &bt_control)
		return ENOTCONN;
	if ((u_long)(unsigned)option != TIOCGETA)
		return ENOPROTOOPT;
	if (data && *length < sizeof(bt_serial))
		return EINVAL;
	*length = sizeof(bt_serial);
	return data ? bt_ioctl(0, TIOCGETA, data, 0, NULL) : 0;
}

static errno_t
bt_control_set(__unused kern_ctl_ref ref, __unused uint32_t unit, void *context,
    int option, void *data, size_t length)
{
	if (context != &bt_control)
		return ENOTCONN;
	u_long command = (unsigned)option;
	if (command != TIOCSETA && command != TIOCSETAW && command != TIOCSETAF)
		return ENOPROTOOPT;
	if (!data || length != sizeof(bt_serial))
		return EINVAL;
	return bt_ioctl(0, command, data, 0, NULL);
}

void
hakios_mtk_bt_attach(void)
{
	static bool registered;
	unsigned native = 0;
	if (registered)
		return;
	int major = cdevsw_add(-1, &bt_switch);
	if (major < 0)
		return;
	(void)cdevsw_setkqueueok(major, &bt_switch, 1);
	PE_parse_boot_argn("mtkbt", &native, sizeof(native));
	bt_serial.c_cflag = CS8 | CREAD | CLOCAL;
	bt_serial.c_ispeed = bt_serial.c_ospeed = B115200;
	if (native) {
		uint8_t address[6];
		lck_mtx_lock(&bt_lock);
		bool valid = bt_prepare() == 0 && bt_read_address(address);
		lck_mtx_unlock(&bt_lock);
		if (!valid || !hakios_mtk_bt_publish_address(address)) {
			bt_note("native Bluetooth address unavailable", 0);
			native = 0;
		} else
			bt_note("published hardware address in IODeviceTree", 0);
	}
	if (native) {
		struct kern_ctl_reg control = {
			.ctl_name = "com.apple.uart.bluetooth",
			.ctl_flags = CTL_FLAG_REG_SOCK_STREAM,
			.ctl_sendsize = BT_RING_BYTES - 1,
			.ctl_recvsize = 64 * 1024,
			.ctl_connect = bt_control_connect,
			.ctl_disconnect = bt_control_disconnect,
			.ctl_send = bt_control_send,
			.ctl_getopt = bt_control_get,
			.ctl_setopt = bt_control_set,
		};
		int error = ctl_register(&control, &bt_control);
		bt_note(error ? "native H4 registration failed" : "native H4 control registered", error);
        if (error) {
            bt_control = NULL;
            native = 0;
        }
	}
	const char *name = native ? "cu.bluetooth" : "hakios-bt";
	if (!devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "%s", name)) {
        if (native) {
            (void)ctl_deregister(bt_control);
            bt_control = NULL;
        }
		(void)cdevsw_remove(major, &bt_switch);
		return;
	}
    registered = true;
    if (native)
        bt_note(hakios_mtk_bt_publish_ready() ? "native HCI ready service published" :
            "native HCI ready service publication failed", 0);
	bt_note(native ? "/dev/cu.bluetooth available" : "/dev/hakios-bt available", 0);
}
