/*
 * duchamp (POCO X6 Pro, MT6897): the GPU, a Mali-G615 MC6, brought up from the kernel. Only with boot-arg mtkgpu=1:
 * every address below belongs to this phone.
 *
 * The G615 is a Valhall GPU with a command stream frontend (CSF): its work goes through a microcontroller of its own,
 * the MCU, that runs Arm's firmware (mali_csffw.bin) and takes queues of commands from memory; and on the MT6897 the
 * GPU's power is not ours to switch but the GPUEB's, a second microcontroller that drives the rails, PLLs and power
 * domains when asked over a mailbox. Every step below was proven first from the boot loader on Isaac's phone
 * (mtkloader/mtkgpu.h, GPU probes 1 to 4b, 12 November; docs/porting-mt6897.md), following Linux's panthor driver
 * (drivers/gpu/drm/panthor: panthor_fw.c, panthor_mmu.c, panthor_gpu.c, panthor_sched.c) and MediaTek's gpufreq and
 * GPUEB drivers (github.com/mt6897-devs/android_kernel_device_modules_6.1, drivers/gpu/mediatek):
 *
 *   1  the GPUEB: its shared memory (CMD_INIT_SHARED_MEM), then CMD_POWER_CONTROL on: rails, PLLs, MFG1 on
 *   2  the firmware's sections copied into our memory and a page table that puts each at its MCU address
 *   3  the GPU: soft reset, no coherency, its L2 on, address space 0 for the MCU, MCU_CONTROL = AUTO; booted, the
 *      firmware fills its global interface
 *   4  a ping to the firmware, and its global setup (cores, timers), as panthor_fw_init_global_iface
 *   5  command stream group 0 with one queue started, in address space 1 (the same table)
 *   6  a five-instruction command stream that writes two words into a page of ours, read back by the processor
 *
 * The boot loader (XNU_GPU=1) leaves what the kernel cannot find by itself on the command line:
 *   mtkgpu=1            this driver runs
 *   mtkgpu_fw=N         the firmware's size: the loader copied it (the payload's "gpufw" entry, the owner's own
 *                       file) to the start of the GPU's memory
 *   mtkgpu_eb=A         the GPUEB's memory (gpueb_mem_addr in LK's device tree, 0x7fe30000 on Isaac's phone)
 *
 * The GPU's memory: 3 MB at 0x230000000, System RAM above XNU's window and the loader's 2 MB (mtkloader/platform.h),
 * that XNU does not manage. It is mapped uncached (ml_io_map): the GPU is not coherent with the processors (its
 * device-tree node says system-coherency = 0), and this way nothing needs a cache cleaned. Device memory takes only
 * aligned accesses, so everything here reads and writes it 32 bits (or a byte) at a time; never bzero or memcpy,
 * which use DC ZVA and pairs (run 20).
 *
 * Afterwards the GPU stays on and the MCU running, with group 0 terminated, for what is to drive it next (the user
 * client and Mesa). sysctl kern.redwood.mtkgpu says how far it got: 6 means every step above worked.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/hakios_panthor.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/* ---- where things are ---- */

#define MALI_PHYS               0x13000000UL    /* mali@13000000; the doorbells are at +0x80000 + 0x10000 each */
#define MALI_SIZE               0xa0000UL       /* the registers and doorbells 0 and 1 */
#define GPUEB_SMEM_PHYS         0x13c2fd80UL    /* the mailbox's shared memory, 0xa0 words */
#define GPUEB_MBOX_PHYS         0x13c62000UL
#define MFG_RPC_PHYS            0x13f91000UL

/*
 * The GPU's memory: 3 MB. Run 25 asked ml_io_map for 64 MB and the kernel panicked (kmem_alloc ... failed with 3, no
 * room): device mappings come out of a small map, so this asks for what is used and no more.
 */
#define MEM_PHYS                0x230000000ULL
#define MEM_SIZE                0x300000UL
#define MEM_FW                  0x0             /* the firmware file, as the loader left it: up to 1 MB */
#define MEM_FW_MAX              0x60000u
#define MEM_PT                  0x60000         /* page tables: 64 pages of 4 KB */
#define MEM_PT_PAGES            64u
#define MEM_SECTIONS            0xa0000         /* the firmware's sections (0xb0000 of them), then the job's pages (0x104000) */

/* the GPUEB's mailbox (gpufreq's IPI pin) */
#define MBOX_SEND_STA           0x00
#define MBOX_SET                0x04
#define MBOX_CLR                0x74
#define MBOX_RECV_STA           0x78
#define GPUFREQ_PIN             1u
#define GPUFREQ_SEND_SLOT       4u
#define GPUFREQ_RECV_SLOT       56u
#define GPUFREQ_WORDS           6u
#define GPUFREQ_MEM_SIZE        0x4000u
#define CMD_INIT_SHARED_MEM     0u
#define CMD_POWER_CONTROL       6u
#define MFG1_PWR_CON            0x70

/* the GPU's registers (panthor_regs.h) */
#define GPU_INT_RAWSTAT         0x20
#define GPU_INT_CLEAR           0x24
#define GPU_CMD                 0x30
#define GPU_STATUS              0x34
#define GPU_FAULT_STATUS        0x3c
#define GPU_FAULT_ADDR_LO       0x40
#define GPU_SHADER_PRESENT_LO   0x100
#define GPU_L2_PRESENT_LO       0x120
#define L2_READY_LO             0x160
#define L2_PWRON_LO             0x1a0
#define L2_PWRTRANS_LO          0x220
#define GPU_COHERENCY_PROTOCOL  0x304
#define MCU_CONTROL             0x700
#define MCU_STATUS              0x704
#define JOB_INT_RAWSTAT         0x1000
#define JOB_INT_CLEAR           0x1004
#define MMU_INT_RAWSTAT         0x2000
#define MMU_INT_CLEAR           0x2004
#define AS(n)                   (0x2400 + 0x40 * (n))
#define AS_TRANSTAB_LO          0x00
#define AS_TRANSTAB_HI          0x04
#define AS_MEMATTR_LO           0x08
#define AS_MEMATTR_HI           0x0c
#define AS_COMMAND              0x18
#define AS_FAULTSTATUS          0x1c
#define AS_FAULTADDRESS_LO      0x20
#define AS_STATUS               0x28
#define AS_TRANSCFG_LO          0x30
#define AS_TRANSCFG_HI          0x34
#define DOORBELL(n)             (0x80000 + 0x10000 * (n))

/* the firmware */
#define CSF_FW_MAGIC            0xc3f13a6eu
#define CSF_SHARED_VA           0x04000000u
#define CSF_MAX_SECTIONS        16u
#define SEC_RD                  (1u << 0)
#define SEC_WR                  (1u << 1)
#define SEC_EX                  (1u << 2)
#define SEC_PROT                (1u << 5)

/*
 * The job's pages, in the MCU's address space: the MCU addresses only 28 bits (probe 4: the queue's page at
 * 0x10001000 was read at 0x1000), so they sit in the shared region's window, as panthor's do.
 */
#define JOB_VA                  0x05000000u
#define JOB_RING_VA             (JOB_VA + 0x0000)
#define JOB_QIN_VA              (JOB_VA + 0x1000)
#define JOB_QOUT_VA             (JOB_VA + 0x2000)
#define JOB_TARGET_VA           (JOB_VA + 0x3000)
#define JOB_SUSPEND_VA          (JOB_VA + 0x4000)
#define JOB_PAGES               (4u + 256u)
#define JOB_MAGIC0              0xc0ffee01u
#define JOB_MAGIC1              0x600dcafeu

SYSCTL_DECL(_kern_redwood);

static vm_offset_t mg_mali, mg_smem, mg_mbox, mg_rpc, mg_mem, mg_eb_mem;
static uint64_t eb_mem_phys;
static uint32_t fw_size, mg_reached, gpu_id, iface_version, mcu_us;
static char mg_failure[160];

SYSCTL_UINT(_kern_redwood, OID_AUTO, mtkgpu, CTLFLAG_RD | CTLFLAG_LOCKED, &mg_reached, 0,
    "how far the Mali-G615 came up: 6 is a command stream run by the GPU");
SYSCTL_UINT(_kern_redwood, OID_AUTO, mtkgpu_id, CTLFLAG_RD | CTLFLAG_LOCKED, &gpu_id, 0, "GPU_ID");
SYSCTL_UINT(_kern_redwood, OID_AUTO, mtkgpu_csf, CTLFLAG_RD | CTLFLAG_LOCKED, &iface_version, 0,
    "the firmware's global interface version");

struct gpu_section {
	uint32_t va, va_end, flags;
	uint32_t off;                   /* in the GPU's memory */
};
static struct gpu_section mg_sections[CSF_MAX_SECTIONS];
static uint32_t mg_nsections, mg_pt_used, mg_mem_next;

static inline uint32_t
mg_rd(vm_offset_t base, uint32_t at)
{
	return *(volatile uint32_t *)(base + at);
}

static inline void
mg_wr(vm_offset_t base, uint32_t at, uint32_t value)
{
	*(volatile uint32_t *)(base + at) = value;
	__asm__ volatile ("dsb sy");
}

static inline void
mg_wr64(vm_offset_t base, uint32_t at, uint64_t value)
{
	mg_wr(base, at, (uint32_t)value);
	mg_wr(base, at + 4, (uint32_t)(value >> 32));
}

/* Until (register & mask) == want, for at most `us` microseconds. */
static bool
mg_wait_for(vm_offset_t base, uint32_t at, uint32_t mask, uint32_t want, uint32_t us)
{
	for (uint32_t t = 0; t < us; t += 10) {
		if ((mg_rd(base, at) & mask) == want) {
			return true;
		}
		delay(10);
	}
	return (mg_rd(base, at) & mask) == want;
}

static void
mg_fail(const char *what)
{
	strlcpy(mg_failure, what, sizeof(mg_failure));
	printf("mtkgpu: %s (step %u of 6)\n", what, mg_reached);
}

/* A device mapping of [phys, phys + size), whatever the page size: XNU's are 16 KB. */
static vm_offset_t
mg_io_map(uint64_t phys, vm_size_t size)
{
	uint64_t base = phys & ~(uint64_t)PAGE_MASK;
	vm_offset_t va = ml_io_map((vm_offset_t)base, round_page(phys + size - base));

	return va ? va + (vm_offset_t)(phys - base) : 0;
}

/* ---- 1: the GPUEB ---- */

static bool
mg_gpufreq_ipi(const uint32_t msg[GPUFREQ_WORDS], uint32_t ans[GPUFREQ_WORDS])
{
	if (!mg_wait_for(mg_mbox, MBOX_SEND_STA, 1u << GPUFREQ_PIN, 0, 500000)) {
		mg_fail("the GPUEB has not taken the last message");
		return false;
	}
	if (mg_rd(mg_mbox, MBOX_RECV_STA) & (1u << GPUFREQ_PIN)) {
		mg_wr(mg_mbox, MBOX_CLR, 1u << GPUFREQ_PIN);
	}
	for (uint32_t i = 0; i < GPUFREQ_WORDS; i++) {
		mg_wr(mg_smem, 4 * (GPUFREQ_SEND_SLOT + i), msg[i]);
	}
	mg_wr(mg_mbox, MBOX_SET, 1u << GPUFREQ_PIN);
	if (!mg_wait_for(mg_mbox, MBOX_RECV_STA, 1u << GPUFREQ_PIN, 1u << GPUFREQ_PIN, 2000000)) {
		mg_fail("the GPUEB did not answer in 2 s");
		return false;
	}
	for (uint32_t i = 0; i < GPUFREQ_WORDS; i++) {
		ans[i] = mg_rd(mg_smem, 4 * (GPUFREQ_RECV_SLOT + i));
	}
	mg_wr(mg_mbox, MBOX_CLR, 1u << GPUFREQ_PIN);
	return true;
}

static bool
mg_power_on(void)
{
	uint32_t msg[GPUFREQ_WORDS] = { 0 }, ans[GPUFREQ_WORDS];

	for (uint32_t i = 0; i < GPUFREQ_MEM_SIZE; i += 4) {
		mg_wr(mg_eb_mem, i, 0);
	}
	msg[0] = CMD_INIT_SHARED_MEM;
	msg[2] = (uint32_t)eb_mem_phys;
	msg[3] = (uint32_t)(eb_mem_phys >> 32);
	msg[4] = GPUFREQ_MEM_SIZE;
	if (!mg_gpufreq_ipi(msg, ans)) {
		return false;
	}
	memset(msg, 0, sizeof(msg));
	msg[0] = CMD_POWER_CONTROL;
	msg[2] = 1;
	if (!mg_gpufreq_ipi(msg, ans)) {
		return false;
	}
	delay(5000);
	printf("mtkgpu: the GPUEB says power count %u; MFG1_PWR_CON %#x\n", ans[2], mg_rd(mg_rpc, MFG1_PWR_CON));
	if (ans[2] < 1 || ((mg_rd(mg_rpc, MFG1_PWR_CON) >> 30) & 3) != 3) {
		mg_fail("the GPU did not come on");
		return false;
	}
	gpu_id = mg_rd(mg_mali, 0);
	printf("mtkgpu: GPU_ID %#x (arch %u.%u, product %u, r%up%u), shader cores %#x, CSF_ID %#x\n", gpu_id,
	    gpu_id >> 28, (gpu_id >> 24) & 15, (gpu_id >> 16) & 15, (gpu_id >> 12) & 15, (gpu_id >> 4) & 15,
	    mg_rd(mg_mali, GPU_SHADER_PRESENT_LO), mg_rd(mg_mali, 0x1c));
	return true;
}

/* ---- 2: the firmware's sections and the MCU's page table ---- */

static inline uint8_t
mg_fw_byte(uint32_t at)
{
	return *(volatile uint8_t *)(mg_mem + MEM_FW + at);
}

static uint64_t
mg_pt_alloc(void)
{
	if (mg_pt_used >= MEM_PT_PAGES) {
		return 0;
	}
	uint32_t off = MEM_PT + 0x1000 * mg_pt_used++;
	for (uint32_t i = 0; i < 0x1000; i += 4) {
		mg_wr(mg_mem, off + i, 0);
	}
	return MEM_PHYS + off;
}

static inline vm_offset_t
mg_pa_to_va(uint64_t pa)
{
	return mg_mem + (vm_offset_t)(pa - MEM_PHYS);
}

/* One 4 KB page, LPAE stage 1 as io-pgtable-arm writes it for panthor; attribute index 0, non-cacheable. */
static bool
mg_map_page(uint64_t l0, uint32_t va, uint64_t pa, uint32_t flags)
{
	uint64_t table = l0;

	for (int level = 0; level < 3; level++) {
		vm_offset_t slot = mg_pa_to_va(table) + 8 * (((uint64_t)va >> (39 - 9 * level)) & 0x1ff);
		uint64_t e = mg_rd(slot, 0) | ((uint64_t)mg_rd(slot, 4) << 32);

		if (!(e & 1)) {
			uint64_t next = mg_pt_alloc();
			if (next == 0) {
				return false;
			}
			e = next | 3;
			mg_wr64(slot, 0, e);
		}
		table = e & 0xfffffffff000ULL;
	}
	uint64_t pte = pa | 3 | (1u << 6) | (2u << 8) | (1u << 10);  /* page, unprivileged, outer shareable, accessed */
	if (!(flags & SEC_WR)) {
		pte |= 2u << 6;                                         /* read only */
	}
	if (!(flags & SEC_EX)) {
		pte |= 3ULL << 53;                                      /* no execute */
	}
	mg_wr64(mg_pa_to_va(table) + 8 * ((va >> 12) & 0x1ff), 0, pte);
	return true;
}

static bool
mg_add_region(uint64_t l0, uint32_t va, uint32_t va_end, uint32_t flags, uint32_t data_at, uint32_t data_len)
{
	uint32_t len = va_end - va;

	if (mg_nsections >= CSF_MAX_SECTIONS || mg_mem_next + len > MEM_SIZE) {
		mg_fail("no room for the firmware's sections");
		return false;
	}
	for (uint32_t i = 0; i < len; i++) {
		*(volatile uint8_t *)(mg_mem + mg_mem_next + i) = i < data_len ? mg_fw_byte(data_at + i) : 0;
	}
	for (uint32_t p = 0; p < len; p += 0x1000) {
		if (!mg_map_page(l0, va + p, MEM_PHYS + mg_mem_next + p, flags)) {
			mg_fail("out of page tables");
			return false;
		}
	}
	mg_sections[mg_nsections].va = va;
	mg_sections[mg_nsections].va_end = va_end;
	mg_sections[mg_nsections].flags = flags;
	mg_sections[mg_nsections].off = mg_mem_next;
	mg_nsections++;
	mg_mem_next += len;
	return true;
}

/* An MCU address to ours, or 0. */
static vm_offset_t
mg_mcu_va(uint32_t va)
{
	for (uint32_t i = 0; i < mg_nsections; i++) {
		if (va >= mg_sections[i].va && va < mg_sections[i].va_end) {
			return mg_mem + mg_sections[i].off + (va - mg_sections[i].va);
		}
	}
	return 0;
}

static uint64_t
mg_load_firmware(void)
{
	uint32_t end, at = 20;
	uint64_t l0;

	if (fw_size < 20 || fw_size > MEM_FW_MAX || mg_rd(mg_mem, MEM_FW) != CSF_FW_MAGIC) {
		mg_fail("no CSF firmware where the loader should have left it (mtkgpu_fw=)");
		return 0;
	}
	end = mg_rd(mg_mem, MEM_FW + 16);             /* the header's size: where the entries stop */
	if (end > fw_size) {
		mg_fail("the firmware's header is bigger than the file");
		return 0;
	}
	mg_pt_used = 0;
	mg_nsections = 0;
	mg_mem_next = MEM_SECTIONS;
	l0 = mg_pt_alloc();
	while (at + 4 <= end) {
		uint32_t ehdr = mg_rd(mg_mem, MEM_FW + at), esz = (ehdr >> 8) & 0xff;

		if (esz < 4 || (esz & 3) || at + esz > end) {
			mg_fail("a bad entry in the firmware");
			return 0;
		}
		if ((ehdr & 0xff) == 0 && esz >= 24) {
			uint32_t flags = mg_rd(mg_mem, MEM_FW + at + 4), va = mg_rd(mg_mem, MEM_FW + at + 8), va_end = mg_rd(mg_mem, MEM_FW + at + 12);
			uint32_t ds = mg_rd(mg_mem, MEM_FW + at + 16), de = mg_rd(mg_mem, MEM_FW + at + 20);

			if (!(flags & SEC_PROT)) {      /* protected mode is not used, as by panthor */
				if ((va & 0xfff) || (va_end & 0xfff) || va_end <= va || de < ds || de > fw_size ||
				    de - ds > va_end - va) {
					mg_fail("a firmware section that cannot be placed");
					return 0;
				}
				if (!mg_add_region(l0, va, va_end, flags, ds, de - ds)) {
					return 0;
				}
			}
		}
		at += esz;
	}
	if (!mg_add_region(l0, JOB_VA, JOB_VA + 0x1000 * JOB_PAGES, SEC_RD | SEC_WR, 0, 0)) {
		return 0;
	}
	if (!mg_mcu_va(CSF_SHARED_VA)) {
		mg_fail("the firmware has no shared section");
		return 0;
	}
	printf("mtkgpu: firmware %u bytes: %u sections, %u page-table pages\n", fw_size, mg_nsections, mg_pt_used);
	return l0;
}

/* ---- 3: the GPU and its MCU ---- */

static void
mg_soft_reset(void)
{
	mg_wr(mg_mali, GPU_INT_CLEAR, 1u << 8);
	mg_wr(mg_mali, GPU_CMD, 0x101);
	if (!mg_wait_for(mg_mali, GPU_INT_RAWSTAT, 1u << 8, 1u << 8, 100000)) {
		printf("mtkgpu: no RESET_COMPLETED in 100 ms\n");
	}
	mg_wr(mg_mali, GPU_INT_CLEAR, ~0u);
	mg_wr(mg_mali, JOB_INT_CLEAR, ~0u);
	mg_wr(mg_mali, MMU_INT_CLEAR, ~0u);
}

static bool
mg_as_enable(uint32_t n, uint64_t l0)
{
	mg_wr(mg_mali, AS(n) + AS_TRANSTAB_LO, (uint32_t)l0);
	mg_wr(mg_mali, AS(n) + AS_TRANSTAB_HI, (uint32_t)(l0 >> 32));
	mg_wr(mg_mali, AS(n) + AS_MEMATTR_LO, 0x4c4c4c4c);  /* all non-cacheable: mair_to_memattr of 0x44 */
	mg_wr(mg_mali, AS(n) + AS_MEMATTR_HI, 0x4c4c4c4c);
	mg_wr(mg_mali, AS(n) + AS_TRANSCFG_LO, (1u << 24) | (7u << 6) | 6u); /* walks NC, 48-bit VA, AARCH64_4K */
	mg_wr(mg_mali, AS(n) + AS_TRANSCFG_HI, 0);
	if (!mg_wait_for(mg_mali, AS(n) + AS_STATUS, 1, 0, 100000)) {
		return false;
	}
	mg_wr(mg_mali, AS(n) + AS_COMMAND, 1);              /* UPDATE */
	return mg_wait_for(mg_mali, AS(n) + AS_STATUS, 1, 0, 100000);
}

static void
mg_show_faults(void)
{
	printf("mtkgpu: MCU_STATUS %u, GPU_STATUS %#x, GPU fault %#x at %#x, MMU %#x, AS0 fault %#x at %#x, "
	    "AS1 fault %#x at %#x\n", mg_rd(mg_mali, MCU_STATUS), mg_rd(mg_mali, GPU_STATUS), mg_rd(mg_mali, GPU_FAULT_STATUS),
	    mg_rd(mg_mali, GPU_FAULT_ADDR_LO), mg_rd(mg_mali, MMU_INT_RAWSTAT), mg_rd(mg_mali, AS(0) + AS_FAULTSTATUS),
	    mg_rd(mg_mali, AS(0) + AS_FAULTADDRESS_LO), mg_rd(mg_mali, AS(1) + AS_FAULTSTATUS), mg_rd(mg_mali, AS(1) + AS_FAULTADDRESS_LO));
}

static vm_offset_t mg_glb_ctl, mg_glb_in, mg_glb_out;

static bool
mg_boot_mcu(uint64_t l0)
{
	uint32_t l2;
	uint64_t t0, t1;

	mg_soft_reset();
	mg_wr(mg_mali, GPU_COHERENCY_PROTOCOL, 31);         /* none */
	l2 = mg_rd(mg_mali, GPU_L2_PRESENT_LO);
	mg_wr(mg_mali, L2_PWRON_LO, l2);
	if (!mg_wait_for(mg_mali, L2_PWRTRANS_LO, l2, 0, 20000) || !mg_wait_for(mg_mali, L2_READY_LO, l2, l2, 20000)) {
		mg_fail("the L2 did not come up");
		return false;
	}
	if (!mg_as_enable(0, l0)) {
		mg_fail("address space 0 stays busy");
		return false;
	}
	mg_wr(mg_mali, JOB_INT_CLEAR, ~0u);
	mg_wr(mg_mali, MCU_CONTROL, 2);                     /* AUTO */
	t0 = mach_absolute_time();
	if (!mg_wait_for(mg_mali, JOB_INT_RAWSTAT, 1u << 31, 1u << 31, 1000000)) {
		mg_show_faults();
		mg_fail("the MCU did not boot in 1 s");
		return false;
	}
	t1 = mach_absolute_time();
	mg_wr(mg_mali, JOB_INT_CLEAR, 1u << 31);
	{
		uint64_t ns;

		absolutetime_to_nanoseconds(t1 - t0, &ns);
		mcu_us = (uint32_t)(ns / 1000);
	}
	mg_glb_ctl = mg_mcu_va(CSF_SHARED_VA);
	iface_version = mg_rd(mg_glb_ctl, 0);
	mg_glb_in = mg_mcu_va(mg_rd(mg_glb_ctl, 8));
	mg_glb_out = mg_mcu_va(mg_rd(mg_glb_ctl, 12));
	printf("mtkgpu: the MCU booted in %u us: CSF interface %u.%u.%u, %u groups, features %#x\n", mcu_us,
	    iface_version >> 24, (iface_version >> 16) & 0xff, iface_version & 0xffff, mg_rd(mg_glb_ctl, 16), mg_rd(mg_glb_ctl, 4));
	if (!mg_glb_in || !mg_glb_out) {
		mg_fail("the global interface is outside the firmware's sections");
		return false;
	}
	return true;
}

/* ---- 4, 5, 6: the firmware's interfaces ---- */

/* req's bits in mask made to differ from ack's (panthor_fw_toggle_reqs) */
static void
mg_toggle(vm_offset_t req, uint32_t ack, uint32_t mask)
{
	mg_wr(req, 0, (mg_rd(req, 0) & ~mask) | ((ack ^ mask) & mask));
}

static bool
mg_wait_ack(vm_offset_t req, vm_offset_t ack, uint32_t mask, uint32_t ms)
{
	for (uint32_t us = 0; us < ms * 1000; us += 10) {
		if ((mg_rd(ack, 0) & mask) == (mg_rd(req, 0) & mask)) {
			return true;
		}
		delay(10);
	}
	return (mg_rd(ack, 0) & mask) == (mg_rd(req, 0) & mask);
}

static uint32_t
mg_fw_timer(uint32_t us)
{
	uint64_t freq = gPEClockFrequencyInfo.timebase_frequency_hz;

	return (uint32_t)((((uint64_t)us * freq + (1000000ULL << 10) - 1) / (1000000ULL << 10)) & 0x7fffffff);
}

static bool
mg_ping(void)
{
	mg_toggle(mg_glb_in, mg_rd(mg_glb_out, 0), 1u << 8);      /* GLB_PING */
	mg_wr(mg_mali, DOORBELL(0), 1);
	if (!mg_wait_ack(mg_glb_in, mg_glb_out, 1u << 8, 100)) {
		mg_fail("the firmware did not answer a ping");
		return false;
	}
	return true;
}

static bool
mg_global_setup(void)
{
	uint32_t cfg = (1u << 2) | (1u << 3) | (1u << 1); /* GLB_CFG_ALLOC_EN, _POWEROFF_TIMER, _PROGRESS_TIMER */

	mg_wr64(mg_glb_in, 0x18, mg_rd(mg_mali, GPU_SHADER_PRESENT_LO));          /* core_en_mask */
	mg_wr(mg_glb_in, 0x14, mg_fw_timer(10000));                            /* poweroff_timer */
	mg_wr(mg_glb_in, 0x10, (uint32_t)((5ULL * 500 * 1024 * 1024) >> 10)); /* progress_timer */
	mg_wr(mg_glb_in, 0x80, mg_fw_timer(800));                              /* idle_timer */
	mg_wr(mg_glb_in, 0x04, cfg | (1u << 8) | (1u << 10) | (1u << 26));  /* ack_irq_mask */
	mg_wr(mg_glb_in, 0, mg_rd(mg_glb_in, 0) | (1u << 10) | (1u << 7));        /* GLB_IDLE_EN, GLB_COUNTER_EN */
	mg_toggle(mg_glb_in, mg_rd(mg_glb_out, 0), cfg);
	mg_wr(mg_mali, DOORBELL(0), 1);
	if (!mg_wait_ack(mg_glb_in, mg_glb_out, cfg, 100)) {
		mg_fail("the firmware did not take its global setup");
		return false;
	}
	return true;
}

static inline uint64_t
mg_cs_instr(uint32_t op, uint64_t data)
{
	return ((uint64_t)op << 56) | (data & 0x00ffffffffffffffULL);
}

static bool
mg_first_stream(uint64_t l0)
{
	vm_offset_t csg_ctl = mg_glb_ctl + 0x1000, cs_ctl = csg_ctl + 0x40;
	vm_offset_t csgi = mg_mcu_va(mg_rd(csg_ctl, 4)), csgo = mg_mcu_va(mg_rd(csg_ctl, 8));
	vm_offset_t csi = mg_mcu_va(mg_rd(cs_ctl, 4)), cso = mg_mcu_va(mg_rd(cs_ctl, 8));
	vm_offset_t ring = mg_mcu_va(JOB_RING_VA), qin = mg_mcu_va(JOB_QIN_VA), qout = mg_mcu_va(JOB_QOUT_VA);
	vm_offset_t target = mg_mcu_va(JOB_TARGET_VA);
	uint32_t shader = mg_rd(mg_mali, GPU_SHADER_PRESENT_LO), n = 0, cores, req, ack;
	bool ran;

	if (!csgi || !csgo || !csi || !cso || mg_rd(csg_ctl, 0xc) > 256 * 0x1000) {
		mg_fail("group 0's interface is not where expected");
		return false;
	}
	if (!mg_as_enable(1, l0)) {
		mg_fail("address space 1 stays busy");
		return false;
	}

	/* r2:r3 = the target; r0, r1 = the words; store both; wait for the store (Mesa genxml v10, "CS" structs) */
	mg_wr64(ring, 8 * n++, mg_cs_instr(1, (2ULL << 48) | JOB_TARGET_VA));              /* MOVE48 */
	mg_wr64(ring, 8 * n++, mg_cs_instr(2, (0ULL << 48) | JOB_MAGIC0));                 /* MOVE32 */
	mg_wr64(ring, 8 * n++, mg_cs_instr(2, (1ULL << 48) | JOB_MAGIC1));                 /* MOVE32 */
	mg_wr64(ring, 8 * n++, mg_cs_instr(21, (0ULL << 48) | (2ULL << 40) | (3ULL << 16))); /* STORE_MULTIPLE */
	mg_wr64(ring, 8 * n++, mg_cs_instr(3, 0xffULL << 16));                              /* WAIT */
	mg_wr64(target, 0, 0);
	mg_wr64(qin, 0, 0);
	mg_wr64(qin, 8, 0);
	mg_wr64(qout, 0, 0);

	/* the queue, as cs_slot_prog_locked: priority 0, doorbell 1 */
	mg_wr64(csi, 0x10, JOB_RING_VA);
	mg_wr(csi, 0x18, 0x1000);
	mg_wr64(csi, 0x30, JOB_QIN_VA);
	mg_wr64(csi, 0x38, JOB_QOUT_VA);
	mg_wr(csi, 0x04, 1u << 8);
	mg_wr(csi, 0x0c, ~0u);
	mg_wr(csi, 0, (mg_rd(csi, 0) & ~((1u << 8) | (1u << 10) | 7u)) | (1u << 8) | (1u << 10) | 1u);

	/* the group, as csg_slot_prog_locked and the scheduler's start; interface < 4.0: a 32-bit endpoint_req */
	cores = (uint32_t)__builtin_popcount(shader);
	mg_wr64(csgi, 0x20, shader);
	mg_wr64(csgi, 0x28, shader);
	mg_wr(csgi, 0x30, 1);
	mg_wr(csgi, 0x34, cores | (cores << 8) | (1u << 16) | (0xfu << 28));
	mg_wr64(csgi, 0x40, JOB_SUSPEND_VA);
	mg_wr64(csgi, 0x48, 0);
	mg_wr(csgi, 0x50, 1);                                                            /* address space 1 */
	mg_wr(csgi, 0x04, ~0u);
	mg_toggle(csgi + 0x08, mg_rd(csgo, 0x08), 1u);
	req = mg_rd(csgi, 0);
	ack = mg_rd(csgo, 0);
	req = (req & ~7u) | 1u;                                                       /* START */
	req = (req & ~(1u << 4)) | ((ack ^ (1u << 4)) & (1u << 4));                   /* ENDPOINT_CONFIG */
	mg_wr(csgi, 0, req);
	mg_toggle(mg_glb_in + 0x08, mg_rd(mg_glb_out, 0x08), 1u);
	mg_wr(mg_mali, DOORBELL(0), 1);
	if (!mg_wait_ack(csgi, csgo, 7u | (1u << 4), 100)) {
		mg_show_faults();
		mg_fail("group 0 did not start");
		return false;
	}
	mg_reached = 5;

	mg_wr64(qin, 0, 8 * n);
	mg_wr(mg_mali, DOORBELL(1), 1);
	for (uint32_t us = 0; us < 500000 && mg_rd(target, 4) != JOB_MAGIC1; us += 10) {
		delay(10);
	}
	ran = mg_rd(target, 0) == JOB_MAGIC0 && mg_rd(target, 4) == JOB_MAGIC1;
	printf("mtkgpu: the command stream %s: target %#x %#x, extract %#x, CS ack %#x, fault %#x, fatal %#x\n",
	    ran ? "RAN ON THE GPU" : "did not run", mg_rd(target, 0), mg_rd(target, 4), mg_rd(qout, 0), mg_rd(cso, 0),
	    mg_rd(cso, 0x80), mg_rd(cso, 0x84));

	/* terminate the group: the slot is free for what comes next */
	mg_wr(csgi, 0, mg_rd(csgi, 0) & ~7u);
	mg_toggle(mg_glb_in + 0x08, mg_rd(mg_glb_out, 0x08), 1u);
	mg_wr(mg_mali, DOORBELL(0), 1);
	if (!mg_wait_ack(csgi, csgo, 7u, 100)) {
		mg_fail("group 0 not terminated in 100 ms; user client withheld");
		return false;
	}
	if (!ran) {
		mg_show_faults();
		mg_fail("the GPU did not run the command stream");
	}
	return ran;
}

/* ---- the thread ---- */

static void
mtkgpu_up(void)
{
	uint64_t l0;

	mg_mali = mg_io_map(MALI_PHYS, MALI_SIZE);
	mg_smem = mg_io_map(GPUEB_SMEM_PHYS, 0xa0 * 4);
	mg_mbox = mg_io_map(GPUEB_MBOX_PHYS, 0x80);
	mg_rpc = mg_io_map(MFG_RPC_PHYS, 0x1000);
	mg_mem = mg_io_map(MEM_PHYS, MEM_SIZE);
	mg_eb_mem = mg_io_map(eb_mem_phys, GPUFREQ_MEM_SIZE);
	if (!mg_mali || !mg_smem || !mg_mbox || !mg_rpc || !mg_mem || !mg_eb_mem) {
		mg_fail("cannot map the GPU's registers or memory");
		return;
	}
	if (!mg_power_on()) {
		return;
	}
	mg_reached = 1;
	l0 = mg_load_firmware();
	if (l0 == 0) {
		return;
	}
	mg_reached = 2;
	if (!mg_boot_mcu(l0)) {
		return;
	}
	mg_reached = 3;
	if (!mg_ping() || !mg_global_setup()) {
		return;
	}
	mg_reached = 4;
	if (mg_first_stream(l0)) {
		mg_reached = 6;
		uint32_t user_client = 0;
		if (PE_parse_boot_argn("mtkgpu_uc", &user_client, sizeof(user_client)) && user_client == 1) {
			hakios_panthor_start(gpu_id, iface_version);
		}
	}
}

static void
mtkgpu_thread(__unused void *parameter, __unused wait_result_t result)
{
	mtkgpu_up();
	printf("mtkgpu: came up to step %u of 6%s%s\n", mg_reached, mg_failure[0] ? ": " : "", mg_failure);
	for (;;) {
		delay(60 * 1000 * 1000);
		/* again, so that it is in the end of a run's log, the part that is kept */
		printf("mtkgpu: step %u of 6 (GPU_ID %#x, CSF %#x, MCU booted in %u us)%s%s\n", mg_reached, gpu_id,
		    iface_version, mcu_us, mg_failure[0] ? ": " : "", mg_failure);
	}
}

void
redwood_mtkgpu_init(void)
{
	uint32_t on = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mtkgpu", &on, sizeof(on)) || on == 0) {
		return;
	}
	PE_parse_boot_argn("mtkgpu_fw", &fw_size, sizeof(fw_size));
	PE_parse_boot_argn("mtkgpu_eb", &eb_mem_phys, sizeof(eb_mem_phys));
	if (fw_size == 0 || eb_mem_phys == 0) {
		printf("mtkgpu: the loader gave no firmware (mtkgpu_fw=) or no GPUEB memory (mtkgpu_eb=)\n");
		return;
	}
	if (kernel_thread_start(mtkgpu_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("mtkgpu: cannot start its thread\n");
	}
}
