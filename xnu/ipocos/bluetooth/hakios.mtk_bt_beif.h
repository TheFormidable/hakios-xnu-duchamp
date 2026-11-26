/* SPDX-License-Identifier: MIT
 * MT6897 BEIF v0 layout, verified against the handset's Android trace.
 */
#ifndef HAKIOS_MTK_BT_BEIF_H
#define HAKIOS_MTK_BT_BEIF_H
#include <stdbool.h>
#include <stdint.h>

enum {
	BT_BEIF_BYTES = 32768, BT_RING_BYTES = 16304,
	BT_HOST_TX = 32, BT_HOST_RX = 36, BT_FW_TX = 64, BT_FW_RX = 68,
	BT_H2C = 96, BT_C2H = 16432
};

static const uint32_t bt_beif_header[] = {
	0x55665566, 0, BT_HOST_TX, BT_FW_TX, BT_H2C, BT_RING_BYTES, BT_C2H, BT_RING_BYTES
};

static inline void
bt_beif_barrier(void)
{
#if defined(__aarch64__)
	__asm__ volatile ("dmb sy" ::: "memory");
#else
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static inline bool
bt_beif_valid(const volatile uint8_t *memory)
{
	const volatile uint32_t *words = (const volatile uint32_t *)memory;

	for (unsigned i = 0; i < sizeof(bt_beif_header) / sizeof(*bt_beif_header); i++) {
		if (words[i] != bt_beif_header[i])
			return false;
	}
	return words[60 / 4] == 0xdeadbeef && words[92 / 4] == 0xdeadbeef &&
	       words[(BT_H2C + BT_RING_BYTES) / 4] == 0xdeadbeef &&
	       words[(BT_C2H + BT_RING_BYTES) / 4] == 0xdeadbeef;
}

static inline void
bt_beif_init(volatile uint8_t *memory)
{
	volatile uint32_t *words = (volatile uint32_t *)memory;

	/* Keep the controller's private control words across a cursor reset. */
	for (unsigned i = 1; i < sizeof(bt_beif_header) / sizeof(*bt_beif_header); i++)
		words[i] = bt_beif_header[i];
	words[BT_HOST_TX / 4] = words[BT_HOST_RX / 4] = 0;
	words[BT_FW_TX / 4] = words[BT_FW_RX / 4] = 0;
	words[60 / 4] = words[92 / 4] = 0xdeadbeef;
	for (unsigned i = 0; i < 8; i++) {
		words[(BT_H2C + BT_RING_BYTES) / 4 + i] = 0xdeadbeef;
		words[(BT_C2H + BT_RING_BYTES) / 4 + i] = 0xdeadbeef;
	}
	bt_beif_barrier();
	words[0] = bt_beif_header[0];
	bt_beif_barrier();
}

static inline int
bt_ring_used(uint32_t producer, uint32_t consumer)
{
	if (producer >= BT_RING_BYTES || consumer >= BT_RING_BYTES)
		return -1;
	return producer >= consumer ? (int)(producer - consumer) :
	       (int)(BT_RING_BYTES - consumer + producer);
}

static inline int
bt_beif_send(volatile uint8_t *memory, const uint8_t *data, unsigned length)
{
	volatile uint32_t *words = (volatile uint32_t *)memory;
	uint32_t head = words[BT_HOST_TX / 4];
	int used = bt_ring_used(head, words[BT_FW_RX / 4]);

	if (used < 0)
		return -1;
	if (length > (unsigned)(BT_RING_BYTES - 1 - used))
		return 0;
	bt_beif_barrier();
	for (unsigned i = 0; i < length; i++) {
		memory[BT_H2C + head++] = data[i];
		if (head == BT_RING_BYTES)
			head = 0;
	}
	bt_beif_barrier();
	words[BT_HOST_TX / 4] = head;
	bt_beif_barrier();
	return (int)length;
}

static inline int
bt_beif_receive(volatile uint8_t *memory, uint8_t *data, unsigned capacity)
{
	volatile uint32_t *words = (volatile uint32_t *)memory;
	uint32_t tail = words[BT_HOST_RX / 4];
	int used = bt_ring_used(words[BT_FW_TX / 4], tail);

	if (used < 0)
		return -1;
	unsigned length = (unsigned)used < capacity ? (unsigned)used : capacity;
	bt_beif_barrier();
	for (unsigned i = 0; i < length; i++) {
		data[i] = memory[BT_C2H + tail++];
		if (tail == BT_RING_BYTES)
			tail = 0;
	}
	bt_beif_barrier();
	words[BT_HOST_RX / 4] = tail;
	return (int)length;
}

/* H4 stream framing. Zero means a partial header, -1 an unknown packet type. */
static inline int
bt_h4_size(const uint8_t *packet, unsigned length)
{
	if (!length)
		return 0;
	switch (packet[0]) {
	case 1: case 3: return length >= 4 ? 4 + packet[3] : 0;
	case 2: return length >= 5 ? 5 + packet[3] + (packet[4] << 8) : 0;
	case 4: return length >= 3 ? 3 + packet[2] : 0;
	case 5: return length >= 5 ? 5 + packet[3] + ((packet[4] & 0x3f) << 8) : 0;
	default: return -1;
	}
}
/* Bounded prefix observer. Payloads are skipped using the real H4 length;
 * fragmented stream reads never become fabricated packet boundaries. */
struct bt_h4_observer {
    uint8_t prefix[8];
    unsigned used, total;
};

static inline void
bt_h4_observe(struct bt_h4_observer *state, const uint8_t *bytes, unsigned length,
    void (*packet)(const uint8_t *, unsigned, void *), void *context)
{
    for (unsigned i = 0; i < length; i++) {
        if (state->used < sizeof(state->prefix))
            state->prefix[state->used] = bytes[i];
        state->used++;
        if (!state->total) {
            int total = bt_h4_size(state->prefix, state->used);
            if (total < 0) { state->used = 0; continue; }
            state->total = (unsigned)total;
        }
        if (state->total && state->used == state->total) {
            packet(state->prefix, state->total, context);
            state->used = state->total = 0;
        }
    }
}
#endif
