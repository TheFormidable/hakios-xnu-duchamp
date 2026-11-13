/*
 * The POCO X6 Pro's UFS (duchamp, MT6897) as a block device: the partition that holds the system image, read when
 * the kernel asks for it -- what bsd/dev/redwood_emmc.c is for the X6826B's eMMC. Boot-arg ufs_root=1 makes it the
 * root (iokit/bsddev/IOKitBSDInit.cpp asks redwood_mtkufs_root for it); /dev/ufs0 and /dev/rufs0 are its nodes once
 * devfs is up.
 *
 * Nothing of the controller is set up here. mtkloader has brought it up the way Android's ufs-mediatek does (UFS
 * clocks, the device reset through MediaTek's SiP call, the host's UniPro/crypto/HCI reset, UniPro's power-down off,
 * DME_LINKSTARTUP, NOP OUT, fDeviceInit -- mtkloader/mtkloader.c, ufs_bringup), found the partition and its APFS
 * containers, and left the host enabled with both request lists stopped. It says where on the command line:
 * ufs_lun= (the logical unit, 2 on this phone), ufs_lba= (the partition's first 4 KB block) and ufs_blocks= (its
 * length in 4 KB blocks). This kernel puts a transfer request list of its own in, starts it, and sends READ(10) and
 * TEST UNIT READY: nothing else is ever built.
 *
 * Read only, by construction: the strategy routine refuses writes, opening for writing is refused, and the only
 * request descriptor this file writes has the direction "from the device" (or none). One request at a time, under a
 * mutex, in slot 0, polled (no interrupt is enabled). The lists, the command descriptor and a 128 KB bounce buffer are
 * one physically contiguous piece of memory mapped a second time without caching (normal non-cacheable, as
 * redwood_lx.c does for what it shares with a device).
 *
 * The logical unit's blocks are 4 KB; this device says 512, as the eMMC one does. A read of whole 4 KB blocks into
 * kernel memory that starts on a cache line (what the buffer cache and the file systems ask for) goes straight into
 * the caller's pages, up to 1 MB per READ(10): the PRDT lists their physical pieces, and the lines are cleaned and
 * invalidated before and after (the controller does not snoop the CPU's caches). Anything else -- a part block, an
 * odd address, a raw read into a user process's memory -- goes through the bounce buffer and is copied.
 *
 * Layouts: JESD223 (UFSHCI: the transfer request descriptor, the PRDT) and JESD220 (UFS: the UPIUs).
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/redwood_mtkufs.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */

#define UFSHCI_PHYS         0x112b0000u         /* ufshci@112b0000, "mediatek,mt8183-ufshci" */
#define UFSHCI_SIZE         0x3000

#define UFS_IS              0x20
#define UFS_HCS             0x30
#define UFS_HCE             0x34
#define UFS_UTRLBA          0x50
#define UFS_UTRLBAU         0x54
#define UFS_UTRLDBR         0x58
#define UFS_UTRLCLR         0x5c
#define UFS_UTRLRSR         0x60
#define UFS_UTRLCNR         0x64
#define UFS_UTMRLBA         0x70
#define UFS_UTMRLBAU        0x74
#define UFS_UTMRLRSR        0x80
#define HCS_DP              0x1u                /* a device is present: the link is up */
#define HCS_LISTS_READY     0x6u                /* both request lists ready for their base addresses */

#define LU_BS               4096u               /* the logical unit's block */
#define DEV_BS              512u                /* the block this device says it has */
#define BOUNCE_SIZE         (128u * 1024u)
#define BOUNCE_BLOCKS       (BOUNCE_SIZE / LU_BS)

/* the one piece of memory shared with the controller */
#define DMA_UTRL            0x0000              /* 32 request descriptors, 1 KB aligned */
#define DMA_UTMRL           0x0400              /* the task list (never started on its own; given for UTMRLRDY) */
#define DMA_UCD             0x0800              /* the command descriptor: UPIU at 0, response at 512, PRDT at 1024 */
#define UCD_RSP             512
#define UCD_PRDT            1024
#define DMA_BOUNCE          0x4000
#define DMA_SIZE            (DMA_BOUNCE + BOUNCE_SIZE)
#define PRDT_MAX            ((DMA_BOUNCE - DMA_UCD - UCD_PRDT) / 16)   /* what fits before the bounce buffer: 832 */
#define PRDT_ENTRY_MAX      (256u * 1024u)      /* an entry's byte count has 18 bits */
#define DIRECT_MAX          (1024u * 1024u)     /* one READ(10) straight into the caller's pages */
#define DIRECT_SPLIT        4096u               /* pieces looked up no bigger than the smallest page */
#define CACHE_LINE          64u

static volatile uint8_t *hci;
static volatile uint8_t *dma;                   /* uncached view */
static uint64_t dma_pa;
static uint8_t ufs_lun = 2;
static uint64_t part_lba, part_blocks;          /* the partition, in 4 KB blocks */
static uint64_t dev_bytes;                      /* the image: its APFS containers, back to back */
static uint8_t tag;
static int ufs_bmajor = -1, ufs_cmajor = -1;
static const char *ufs_failed;
static uint32_t ufs_failed_status;
static uint64_t ufs_reads, ufs_read_blocks, ufs_errors, ufs_direct_reads;
static uint32_t prdt_n;                         /* PRDT entries the next request uses */
static uint64_t prdt_last_pa;                   /* the last one's address and length, to join a piece to it */
static uint32_t prdt_last_len;
static LCK_GRP_DECLARE(ufs_lck_grp, "redwood-mtkufs");
static LCK_MTX_DECLARE(ufs_lock, &ufs_lck_grp);

static inline uint32_t
rd(uint32_t reg)
{
	return *(volatile uint32_t *)(hci + reg);
}

static inline void
wr(uint32_t reg, uint32_t value)
{
	*(volatile uint32_t *)(hci + reg) = value;
}

static bool
fail(const char *what, uint32_t status)
{
	ufs_failed = what;
	ufs_failed_status = status;
	ufs_errors++;
	return false;
}

static bool
wait_reg(uint32_t reg, uint32_t mask, uint32_t want, uint32_t ms)
{
	uint64_t end, now;

	nanoseconds_to_absolutetime((uint64_t)ms * NSEC_PER_MSEC, &end);
	end += mach_absolute_time();
	while ((rd(reg) & mask) != want) {
		now = mach_absolute_time();
		if (now > end) {
			return false;
		}
		delay(2);
	}
	return true;
}

/* ---- one request ---- */

/*
 * The command descriptor holds a READ(10) or TEST UNIT READY for ufs_lun; slot 0 takes it, from the device or with no
 * data, into what the PRDT (prdt_n entries) lists. Returns the SCSI status (0 good, 2 check condition), or -1 when
 * nothing usable came back.
 */
static int
request(uint32_t len)
{
	volatile uint32_t *d = (volatile uint32_t *)(dma + DMA_UTRL);
	volatile uint8_t *rsp = dma + DMA_UCD + UCD_RSP;
	uint64_t ucd = dma_pa + DMA_UCD;
	uint32_t ocs;

	if (rd(UFS_UTRLDBR) & 1) {
		return fail("slot 0 still busy", rd(UFS_UTRLDBR)), -1;
	}
	d[0] = (1u << 28) | ((len ? 2u : 0u) << 25) | (1u << 24);    /* UFS storage, from the device or none */
	d[1] = 0;
	d[2] = 0x0f;                                    /* OCS "invalid" until the controller writes it */
	d[3] = 0;
	d[4] = (uint32_t)ucd;
	d[5] = (uint32_t)(ucd >> 32);
	d[6] = (UCD_RSP / 4) << 16 | (512 / 4);         /* the response UPIU: offset, length (in words) */
	d[7] = (UCD_PRDT / 4) << 16 | (len ? prdt_n : 0);
	for (int i = 0; i < 512 / 4; i++) {
		((volatile uint32_t *)rsp)[i] = 0;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(UFS_IS, rd(UFS_IS));
	wr(UFS_UTRLDBR, 1);
	if (!wait_reg(UFS_UTRLDBR, 1, 0, 2000)) {
		wr(UFS_UTRLCLR, ~1u);
		wait_reg(UFS_UTRLDBR, 1, 0, 100);
		return fail("no completion in 2 s", rd(UFS_IS)), -1;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(UFS_UTRLCNR, rd(UFS_UTRLCNR));
	ocs = d[2] & 0xff;
	if (ocs != 0) {
		return fail("the controller's status (OCS)", ocs), -1;
	}
	if (rsp[0] != 0x21) {
		return fail("no response UPIU", rsp[0]), -1;
	}
	if (rsp[7] != 0) {
		/* fixed-format sense after its length at 32: key, ASC, ASCQ */
		fail("check condition (sense key, ASC, ASCQ)", (uint32_t)(rsp[36] & 0xf) << 16 | (uint32_t)rsp[46] << 8 | rsp[47]);
	}
	return rsp[7];
}

/* a fresh command UPIU in the descriptor, for ufs_lun, reading len bytes */
static volatile uint8_t *
command(uint32_t len)
{
	volatile uint8_t *u = dma + DMA_UCD;

	for (int i = 0; i < 512 / 4; i++) {
		((volatile uint32_t *)u)[i] = 0;
	}
	u[0] = 0x01;                                    /* COMMAND */
	u[1] = len ? 0x40 : 0;                          /* R (never W), simple task */
	u[2] = ufs_lun;
	u[3] = tag++ & 0x1f;
	u[12] = (uint8_t)(len >> 24);
	u[13] = (uint8_t)(len >> 16);
	u[14] = (uint8_t)(len >> 8);
	u[15] = (uint8_t)len;
	return u + 16;                                  /* the CDB */
}

static int
test_unit_ready(void)
{
	(void)command(0);                               /* CDB all zero: TEST UNIT READY */
	return request(0);
}

/* the PRDT: the bounce buffer alone */
static void
prdt_bounce(uint32_t len)
{
	volatile uint32_t *prdt = (volatile uint32_t *)(dma + DMA_UCD + UCD_PRDT);
	uint64_t buf = dma_pa + DMA_BOUNCE;

	prdt[0] = (uint32_t)buf;
	prdt[1] = (uint32_t)(buf >> 32);
	prdt[2] = 0;
	prdt[3] = len - 1;
	prdt_n = 1;
}

/* one more physical piece of the destination: joined to the last entry when it follows it, else an entry of its own */
static bool
prdt_add(uint64_t pa, uint32_t len)
{
	volatile uint32_t *e;

	if (prdt_n > 0 && prdt_last_pa + prdt_last_len == pa && prdt_last_len + len <= PRDT_ENTRY_MAX) {
		prdt_last_len += len;
		e = (volatile uint32_t *)(dma + DMA_UCD + UCD_PRDT) + 4 * (prdt_n - 1);
		e[3] = prdt_last_len - 1;
		return true;
	}
	if (prdt_n == PRDT_MAX) {
		return false;
	}
	e = (volatile uint32_t *)(dma + DMA_UCD + UCD_PRDT) + 4 * prdt_n++;
	e[0] = (uint32_t)pa;
	e[1] = (uint32_t)(pa >> 32);
	e[2] = 0;
	e[3] = len - 1;
	prdt_last_pa = pa;
	prdt_last_len = len;
	return true;
}

/* READ(10) of blocks of the logical unit from lba, into what the PRDT lists; three tries */
static bool
read_cmd(uint64_t lba, uint32_t blocks)
{
	for (int tries = 0; tries < 3; tries++) {
		volatile uint8_t *cdb = command(blocks * LU_BS);
		int status;

		cdb[0] = 0x28;                          /* READ(10) */
		cdb[2] = (uint8_t)(lba >> 24);
		cdb[3] = (uint8_t)(lba >> 16);
		cdb[4] = (uint8_t)(lba >> 8);
		cdb[5] = (uint8_t)lba;
		cdb[7] = (uint8_t)(blocks >> 8);
		cdb[8] = (uint8_t)blocks;
		status = request(blocks * LU_BS);
		if (status == 0) {
			ufs_reads++;
			ufs_read_blocks += blocks;
			return true;
		}
		if (status < 0) {
			break;
		}
	}
	return false;
}

/* blocks (at most BOUNCE_BLOCKS) of the logical unit, from lba, into the bounce buffer */
static bool
read_lu(uint64_t lba, uint32_t blocks)
{
	prdt_bounce(blocks * LU_BS);
	return read_cmd(lba, blocks);
}

static inline void
dcache_clean_invalidate(vm_offset_t va, vm_size_t len)
{
	for (vm_offset_t line = va & ~(vm_offset_t)(CACHE_LINE - 1); line < va + len; line += CACHE_LINE) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/*
 * blocks of the partition from its block first, straight into dst: kernel memory starting on a cache line, at most
 * DIRECT_MAX bytes. Its lines are cleaned and invalidated before (nothing dirty may be written back over what the
 * device puts there) and after (nothing the CPU fetched meanwhile may stand for it).
 */
static bool
read_direct(uint64_t first, uint32_t blocks, uint8_t *dst)
{
	vm_offset_t va = (vm_offset_t)dst, end = va + (vm_offset_t)blocks * LU_BS;
	bool ok;

	if (first + blocks > part_blocks) {
		return fail("past the partition's end", (uint32_t)first), false;
	}
	prdt_n = 0;
	while (va < end) {
		vm_offset_t next = (va & ~(vm_offset_t)(DIRECT_SPLIT - 1)) + DIRECT_SPLIT;
		uint32_t n = (uint32_t)((next < end ? next : end) - va);
		uint64_t pa = kvtophys(va);

		if (pa == 0) {
			return fail("a page of the destination has no physical address", (uint32_t)(va - (vm_offset_t)dst)), false;
		}
		if (!prdt_add(pa, n)) {
			return fail("the destination is in too many pieces", prdt_n), false;
		}
		va += n;
	}
	dcache_clean_invalidate((vm_offset_t)dst, (vm_size_t)blocks * LU_BS);
	ok = read_cmd(part_lba + first, blocks);
	dcache_clean_invalidate((vm_offset_t)dst, (vm_size_t)blocks * LU_BS);
	if (ok) {
		ufs_direct_reads++;
	}
	return ok;
}

/*
 * len bytes of the partition from byte off: through the bounce buffer, a whole block at a time; into kernel memory at
 * dst, or with copyout into the calling process's memory at udst (when udst is not 0)
 */
static bool
read_bounced(uint64_t off, uint8_t *dst, user_addr_t udst, uint64_t len)
{
	while (len > 0) {
		uint64_t first = off / LU_BS, skip = off % LU_BS;
		uint64_t blocks = (skip + len + LU_BS - 1) / LU_BS, n;

		if (blocks > BOUNCE_BLOCKS) {
			blocks = BOUNCE_BLOCKS;
		}
		if (first + blocks > part_blocks) {
			return fail("past the partition's end", (uint32_t)first), false;
		}
		if (!read_lu(part_lba + first, (uint32_t)blocks)) {
			return false;
		}
		n = blocks * LU_BS - skip;
		if (n > len) {
			n = len;
		}
		if (udst != 0) {
			if (copyout((const void *)(uintptr_t)(dma + DMA_BOUNCE + skip), udst, (size_t)n) != 0) {
				return fail("copyout to the process", (uint32_t)n), false;
			}
			udst += n;
		} else {
			memcpy(dst, (const void *)(uintptr_t)(dma + DMA_BOUNCE + skip), (size_t)n);
			dst += n;
		}
		off += n;
		len -= n;
	}
	return true;
}

/* len bytes of the partition from byte off into kernel memory: straight in when it can be, else bounced */
static bool
read_bytes(uint64_t off, uint8_t *dst, uint64_t len)
{
	if (off % LU_BS != 0 || len % LU_BS != 0 || ((uintptr_t)dst & (CACHE_LINE - 1)) != 0) {
		return read_bounced(off, dst, 0, len);
	}
	while (len > 0) {
		uint32_t n = len < DIRECT_MAX ? (uint32_t)len : DIRECT_MAX;

		if (!read_direct(off / LU_BS, n / LU_BS, dst)) {
			return false;
		}
		off += n;
		dst += n;
		len -= n;
	}
	return true;
}

/* ---- the controller, as the loader left it ---- */

static bool
take_controller(void)
{
	vm_offset_t cached = 0;
	uint32_t hcs;

	hci = (volatile uint8_t *)ml_io_map(UFSHCI_PHYS, UFSHCI_SIZE);
	if (hci == NULL) {
		return fail("no mapping of the controller", 0);
	}
	hcs = rd(UFS_HCS);
	if (rd(UFS_HCE) != 1 || !(hcs & HCS_DP)) {
		printf("mtkufs: the controller is not up (HCE %#x, HCS %#x): the loader brings it up (ufs_root=1)\n",
		    rd(UFS_HCE), hcs);
		return fail("controller not up", hcs);
	}
	if (rd(UFS_UTRLRSR) & 1) {
		printf("mtkufs: a request list is running already (UTRLBA %#x): not taking it over\n", rd(UFS_UTRLBA));
		return fail("list running", rd(UFS_UTRLBA));
	}
	if (kmem_alloc_contig(kernel_map, &cached, DMA_SIZE, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return fail("no memory in one piece", DMA_SIZE);
	}
	dma_pa = kvtophys(cached);
	bzero((void *)cached, DMA_SIZE);
	for (vm_offset_t line = cached; line < cached + DMA_SIZE; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	dma = (volatile uint8_t *)ml_io_map_unmappable(dma_pa, DMA_SIZE, VM_WIMG_WCOMB);
	if (dma == NULL) {
		return fail("no uncached mapping", 0);
	}
	wr(UFS_UTRLBA, (uint32_t)(dma_pa + DMA_UTRL));
	wr(UFS_UTRLBAU, (uint32_t)((dma_pa + DMA_UTRL) >> 32));
	wr(UFS_UTMRLBA, (uint32_t)(dma_pa + DMA_UTMRL));
	wr(UFS_UTMRLBAU, (uint32_t)((dma_pa + DMA_UTMRL) >> 32));
	if (!wait_reg(UFS_HCS, HCS_LISTS_READY, HCS_LISTS_READY, 100)) {
		printf("mtkufs: the request lists are not ready (HCS %#x)\n", rd(UFS_HCS));
		return fail("lists not ready", rd(UFS_HCS));
	}
	wr(UFS_UTMRLRSR, 1);
	wr(UFS_UTRLRSR, 1);
	for (int i = 0; i < 3; i++) {
		if (test_unit_ready() == 0) {
			printf("mtkufs: controller taken over (lists at %#llx), LUN %u ready\n", dma_pa, ufs_lun);
			return true;
		}
	}
	printf("mtkufs: LUN %u not ready: %s (%#x)\n", ufs_lun, ufs_failed ? ufs_failed : "?", ufs_failed_status);
	return false;
}

/* the partition, from the command line */
static bool
parse_args(void)
{
	uint64_t lba = 0, blocks = 0, lun = 2;

	if (!PE_parse_boot_argn("ufs_lba", &lba, sizeof(lba)) || !PE_parse_boot_argn("ufs_blocks", &blocks, sizeof(blocks)) ||
	    lba == 0 || blocks == 0) {
		printf("mtkufs: no ufs_lba= / ufs_blocks= on the command line (the loader puts them there)\n");
		return false;
	}
	if (PE_parse_boot_argn("ufs_lun", &lun, sizeof(lun)) && lun < 8) {
		ufs_lun = (uint8_t)lun;
	}
	part_lba = lba;
	part_blocks = blocks;
	return true;
}

/* the device is as long as the APFS containers in the partition */
static bool
find_image(void)
{
	uint8_t *block;
	uint64_t at = 0;

	block = kalloc_data(LU_BS, Z_WAITOK);
	if (block == NULL) {
		return false;
	}
	while (at < part_blocks * LU_BS) {
		uint32_t magic, bs;
		uint64_t n;

		if (!read_bytes(at, block, LU_BS)) {
			break;
		}
		memcpy(&magic, block + 32, 4);
		memcpy(&bs, block + 36, 4);
		memcpy(&n, block + 40, 8);
		if (magic != 0x4253584eu || bs < 512 || bs > 65536 || n == 0 || n * bs > part_blocks * LU_BS - at) {   /* "NXSB" */
			break;
		}
		at += n * bs;
	}
	kfree_data(block, LU_BS);
	if (at == 0) {
		printf("mtkufs: no APFS container at block %llu of LUN %u (%s)\n", part_lba, ufs_lun,
		    ufs_failed ? ufs_failed : "no NXSB");
		return false;
	}
	dev_bytes = at;
	printf("mtkufs: the system image at block %llu of LUN %u: %llu MB in its APFS containers, of %llu MB\n", part_lba,
	    ufs_lun, dev_bytes >> 20, (part_blocks * LU_BS) >> 20);
	return true;
}

/* ---- the devices ---- */

static int
ufs_open(__unused dev_t dev, int flags, __unused int devtype, __unused proc_t p)
{
	return (flags & FWRITE) ? EROFS : 0;
}

static int
ufs_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused proc_t p)
{
	return 0;
}

static void
ufs_strategy(buf_t bp)
{
	uint64_t off = (uint64_t)buf_blkno(bp) * DEV_BS;
	uint32_t count = buf_count(bp), done = 0;
	uintptr_t data = buf_dataptr(bp);
	caddr_t mapped = NULL;
	bool user, ok;
	int error = 0;

	if (!(buf_flags(bp) & B_READ)) {
		error = EROFS;                      /* this device never writes */
		goto out;
	}
	if (off >= dev_bytes) {
		buf_setresid(bp, count);
		error = off == dev_bytes ? 0 : EINVAL;
		goto out;
	}
	if (count > dev_bytes - off) {
		count = (uint32_t)(dev_bytes - off);
	}
	/* physio's buffer into a process's memory (rufs0 read from user space), or kernel memory buf_map gives */
	user = (buf_flags(bp) & B_PHYS) && data < VM_MIN_KERNEL_ADDRESS;
	if (!user && (buf_map(bp, &mapped) != 0 || mapped == NULL)) {
		printf("mtkufs: byte %llu: the buffer could not be mapped\n", off);
		error = EIO;
		goto out;
	}
	lck_mtx_lock(&ufs_lock);
	ok = user ? read_bounced(off, NULL, (user_addr_t)data, count) : read_bytes(off, (uint8_t *)mapped, count);
	lck_mtx_unlock(&ufs_lock);
	if (!user) {
		buf_unmap(bp);
	}
	if (ok) {
		done = count;
	} else {
		error = EIO;
	}
	buf_setresid(bp, buf_count(bp) - done);
	if (error) {
		printf("mtkufs: byte %llu, %u bytes: %s (%#x)\n", off, count, ufs_failed ? ufs_failed : "?", ufs_failed_status);
	}
out:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int
ufs_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, __unused proc_t p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*(uint32_t *)data = DEV_BS;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == DEV_BS ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = dev_bytes / DEV_BS;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = DIRECT_MAX / DEV_BS;
		return 0;
	default:
		return ENOTTY;
	}
}

static int
ufs_size(__unused dev_t dev)
{
	return DEV_BS;
}

static int
ufs_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(ufs_strategy, NULL, dev, B_READ, (u_int (*)(buf_t))ufs_size, uio, DEV_BS);
}

static const struct bdevsw ufs_bdevsw = {
	.d_open     = ufs_open,
	.d_close    = ufs_close,
	.d_strategy = ufs_strategy,
	.d_ioctl    = ufs_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = ufs_size,
	.d_type     = D_DISK,
};

static const struct cdevsw ufs_cdevsw = {
	.d_open       = ufs_open,
	.d_close      = ufs_close,
	.d_read       = ufs_read,
	.d_write      = eno_rdwrt,
	.d_ioctl      = ufs_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

int
redwood_mtkufs_root(dev_t *root)
{
	uint64_t t0, ns;
	uint8_t *probe;

	if (ufs_bmajor >= 0) {
		*root = makedev(ufs_bmajor, 0);
		return 0;
	}
	lck_mtx_lock(&ufs_lock);
	if (!parse_args() || !take_controller() || !find_image()) {
		lck_mtx_unlock(&ufs_lock);
		return ENXIO;
	}
	/* how fast: the first 4 MB, which the root's mount reads anyway */
	probe = kalloc_data(BOUNCE_SIZE, Z_WAITOK);
	if (probe != NULL) {
		t0 = mach_absolute_time();
		for (uint64_t at = 0; at < 4 * 1024 * 1024 && at + BOUNCE_SIZE <= dev_bytes; at += BOUNCE_SIZE) {
			if (!read_bytes(at, probe, BOUNCE_SIZE)) {
				break;
			}
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("mtkufs: 4 MB in %llu ms\n", ns / 1000000);
		kfree_data(probe, BOUNCE_SIZE);
	}
	lck_mtx_unlock(&ufs_lock);
	ufs_bmajor = bdevsw_add(-1, &ufs_bdevsw);
	if (ufs_bmajor < 0 || (ufs_cmajor = cdevsw_add_with_bdev(-1, &ufs_cdevsw, ufs_bmajor)) < 0) {
		printf("mtkufs: no room among the devices (%d, %d)\n", ufs_bmajor, ufs_cmajor);
		return ENXIO;
	}
	*root = makedev(ufs_bmajor, 0);
	return 0;
}

/*
 * The test's second half: the same bytes straight into kernel pages, DIRECT_MAX at a time, as the root's reads go --
 * the same sum, or the direct path is wrong -- then up to 64 MB that way, for the speed.
 */
static void
test_direct(uint64_t len, uint64_t want)
{
	uint64_t sum = 0xcbf29ce484222325ULL, ns, t0, at, all;
	vm_offset_t big = 0;

	if (kmem_alloc(kernel_map, &big, DIRECT_MAX, KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mtkufs: test: no 1 MB for the direct reads\n");
		return;
	}
	t0 = mach_absolute_time();
	for (at = 0; at < len; at += DIRECT_MAX) {
		uint64_t n = len - at < DIRECT_MAX ? len - at : DIRECT_MAX;

		if (!read_bytes(at, (uint8_t *)big, n)) {
			break;
		}
		for (uint64_t i = 0; i < n; i += 8) {
			uint64_t w;

			memcpy(&w, (const uint8_t *)big + i, 8);
			sum = (sum ^ w) * 0x100000001b3ULL;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	if (at < len) {
		printf("mtkufs: test: direct: FAILED at byte %llu: %s (%#x)\n", at, ufs_failed ? ufs_failed : "?",
		    ufs_failed_status);
	} else {
		printf("mtkufs: test: direct: %llu KB in %llu ms, sum %016llx (%s)\n", len >> 10, ns / 1000000, sum,
		    sum == want ? "the same" : "NOT THE SAME");
		all = part_blocks * LU_BS < 64 * 1024 * 1024 ? part_blocks * LU_BS : 64 * 1024 * 1024;
		all -= all % DIRECT_MAX;
		t0 = mach_absolute_time();
		for (at = 0; at < all && read_bytes(at, (uint8_t *)big, DIRECT_MAX); at += DIRECT_MAX) {
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("mtkufs: test: direct: %llu MB in %llu ms%s (%llu MB/s)\n", at >> 20, ns / 1000000,
		    at < all ? " (stopped by an error)" : "", ns ? (at * 1000) / ns : 0);
	}
	kmem_free(kernel_map, big, DIRECT_MAX);
}

/* ---- ufs_test=2: the long read ---- */

#define BIG_GRAN            (64u * 1024u)       /* the first pass keeps a sum of every 64 KB; the re-reads are checked against them */
#define BIG_MB_DEFAULT      12288u              /* all of it read, in MB: the partition once (up to this), the rest re-reads */
#define BIG_REREAD_MIN      1024u               /* at least this many MB of re-reads, however big the partition */
#define BIG_SHOW_BAD        8u

static uint64_t
fnv(const uint8_t *p, uint64_t len)
{
	uint64_t sum = 0xcbf29ce484222325ULL;

	for (uint64_t i = 0; i < len; i += 8) {
		uint64_t w;

		memcpy(&w, p + i, 8);
		sum = (sum ^ w) * 0x100000001b3ULL;
	}
	return sum;
}

static uint64_t big_rng = 0x9e3779b97f4a7c15ULL;

static uint64_t
big_rand(void)
{
	/* xorshift64*: fixed seed, so a run can be repeated read for read */
	big_rng ^= big_rng >> 12;
	big_rng ^= big_rng << 25;
	big_rng ^= big_rng >> 27;
	return big_rng * 0x2545f4914f6cdd1dULL;
}

static uint64_t
ms_since(uint64_t t0)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns / 1000000;
}

/*
 * The partition from its start, DIRECT_MAX at a time, as the root's reads go, up to ufs_big_mb= MB (12 GB unless the
 * command line says otherwise): its SHA-256 (Android's sha256sum over the same bytes must print the same) and a sum
 * of every 64 KB. Then whatever is left of ufs_big_mb (at least 1 GB) as re-reads of random places, each of 64 KB to
 * 1 MB, in one of four shapes -- one READ(10) into a buffer that starts on any cache line; pieces of 4 KB to 256 KB;
 * pieces of any length (part blocks, through the bounce buffer); all of it to an odd address -- each 64 KB checked
 * against the first pass. Read only, as everything here.
 */
static void
test_big(void)
{
	static const char *const shape[4] = { "one read", "4 KB pieces", "odd pieces", "odd address" };
	uint64_t total_mb = BIG_MB_DEFAULT, seq, reread, ngran, at, t0, t_read = 0, t_hash = 0, done = 0, bad = 0;
	uint64_t rounds = 0, by_shape[4] = { 0 }, errors0 = ufs_errors;
	vm_offset_t big = 0, sums_va = 0;
	vm_size_t sums_size;
	uint64_t *sums;
	uint8_t digest[SHA256_DIGEST_LENGTH];
	SHA256_CTX ctx;
	bool ok = true;

	PE_parse_boot_argn("ufs_big_mb", &total_mb, sizeof(total_mb));
	seq = part_blocks * LU_BS;
	if (seq > total_mb << 20) {
		seq = total_mb << 20;
	}
	seq -= seq % BIG_GRAN;
	reread = (total_mb << 20) > seq ? (total_mb << 20) - seq : 0;
	if (reread < ((uint64_t)BIG_REREAD_MIN << 20)) {
		reread = (uint64_t)BIG_REREAD_MIN << 20;
	}
	ngran = seq / BIG_GRAN;
	if (ngran == 0) {
		printf("mtkufs: big: the partition is smaller than 64 KB\n");
		return;
	}
	sums_size = round_page(ngran * sizeof(uint64_t));
	if (kmem_alloc(kernel_map, &big, DIRECT_MAX + PAGE_SIZE, KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &sums_va, sums_size, KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mtkufs: big: no memory for the buffers\n");
		if (big) {
			kmem_free(kernel_map, big, DIRECT_MAX + PAGE_SIZE);
		}
		return;
	}
	sums = (uint64_t *)sums_va;
	printf("mtkufs: big (ufs_test=2): %llu MB of the partition from its start, SHA-256, then %llu MB of re-reads\n",
	    seq >> 20, reread >> 20);

	/* the first pass */
	SHA256_Init(&ctx);
	for (at = 0; at < seq; at += DIRECT_MAX) {
		uint64_t n = seq - at < DIRECT_MAX ? seq - at : DIRECT_MAX, t;

		t = mach_absolute_time();
		if (!read_bytes(at, (uint8_t *)big, n)) {
			ok = false;
			break;
		}
		t_read += mach_absolute_time() - t;
		t = mach_absolute_time();
		SHA256_Update(&ctx, (const void *)big, (size_t)n);
		for (uint64_t g = 0; g < n; g += BIG_GRAN) {
			sums[(at + g) / BIG_GRAN] = fnv((const uint8_t *)big + g, BIG_GRAN);
		}
		t_hash += mach_absolute_time() - t;
		if (((at + n) & ((1ULL << 30) - 1)) == 0) {
			uint64_t r, h;

			absolutetime_to_nanoseconds(t_read, &r);
			absolutetime_to_nanoseconds(t_hash, &h);
			printf("mtkufs: big: %llu MB: reading %llu ms, summing %llu ms\n", (at + n) >> 20, r / 1000000,
			    h / 1000000);
		}
	}
	if (!ok) {
		printf("mtkufs: big: FAILED at byte %llu: %s (%#x); %llu errors\n", at, ufs_failed ? ufs_failed : "?",
		    ufs_failed_status, ufs_errors - errors0);
		goto out;
	}
	SHA256_Final(digest, &ctx);
	{
		char hex[2 * SHA256_DIGEST_LENGTH + 1];
		uint64_t r, h;

		for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
			snprintf(hex + 2 * i, 3, "%02x", digest[i]);
		}
		absolutetime_to_nanoseconds(t_read, &r);
		absolutetime_to_nanoseconds(t_hash, &h);
		printf("mtkufs: big: %llu MB read in %llu ms (%llu MB/s), summed in %llu ms\n", seq >> 20, r / 1000000,
		    r ? (seq * 1000) / r : 0, h / 1000000);
		printf("mtkufs: big: SHA-256 of the first %llu MB (%llu bytes): %s\n", seq >> 20, seq, hex);
	}

	/* the re-reads */
	t0 = mach_absolute_time();
	while (done < reread && ok) {
		uint64_t g = big_rand() % ngran, cnt = 1 + big_rand() % (DIRECT_MAX / BIG_GRAN), s = big_rand() % 4;
		uint64_t off, len, lead;
		uint8_t *dst;

		if (cnt > ngran - g) {
			cnt = ngran - g;
		}
		off = g * BIG_GRAN;
		len = cnt * BIG_GRAN;
		lead = s == 3 ? 1 + big_rand() % (CACHE_LINE - 1) : (big_rand() % (PAGE_SIZE / CACHE_LINE)) * CACHE_LINE;
		dst = (uint8_t *)big + lead;
		if (s == 0 || s == 3) {
			ok = read_bytes(off, dst, len);
		} else {
			for (uint64_t p = 0, n; p < len && ok; p += n) {
				n = s == 1 ? LU_BS * (1 + big_rand() % 64) : 1 + big_rand() % (256u * 1024u);
				if (n > len - p) {
					n = len - p;
				}
				ok = read_bytes(off + p, dst + p, n);
			}
		}
		if (!ok) {
			printf("mtkufs: big: re-read FAILED at byte %llu (%s, %llu bytes): %s (%#x)\n", off, shape[s], len,
			    ufs_failed ? ufs_failed : "?", ufs_failed_status);
			break;
		}
		for (uint64_t k = 0; k < cnt; k++) {
			uint64_t now = fnv(dst + k * BIG_GRAN, BIG_GRAN);

			if (now != sums[g + k]) {
				if (bad < BIG_SHOW_BAD) {
					printf("mtkufs: big: NOT THE SAME at byte %llu (%s, from byte %llu, buffer +%llu): %016llx, "
					    "the first pass %016llx\n", (g + k) * BIG_GRAN, shape[s], off, lead, now, sums[g + k]);
				}
				bad++;
			}
		}
		done += len;
		rounds++;
		by_shape[s]++;
	}
	printf("mtkufs: big: re-reads: %llu MB in %llu ms, %llu reads (%llu one read, %llu 4 KB pieces, %llu odd pieces, "
	    "%llu odd address), %llu of %llu x 64 KB not the same\n", done >> 20, ms_since(t0), rounds, by_shape[0],
	    by_shape[1], by_shape[2], by_shape[3], bad, done / BIG_GRAN);
	printf("mtkufs: big: %s: %llu MB read in all, %llu errors\n", ok && bad == 0 ? "PASSED" : "FAILED",
	    (seq + done) >> 20, ufs_errors - errors0);
out:
	kmem_free(kernel_map, sums_va, sums_size);
	kmem_free(kernel_map, big, DIRECT_MAX + PAGE_SIZE);
}

/*
 * ufs_test=1: the controller taken over and the partition read, without making it the root (rd=md0 stays the root).
 * The sum over its first 4 MB is the one mtkloader printed before the kernel started (ufs_sum there): the two must be
 * the same. Then up to 16 MB of it, for the speed. ufs_test=2: then the long read (test_big: 12 GB, the SHA-256 of the
 * partition and re-reads checked against it).
 */
void
redwood_mtkufs_test(void)
{
	uint64_t sum = 0xcbf29ce484222325ULL, ns, t0, len, at;
	uint32_t mode = 1;
	uint8_t *buf;

	PE_parse_boot_argn("ufs_test", &mode, sizeof(mode));
	printf("mtkufs: test (ufs_test=%u): reading the partition the loader named, nothing else\n", mode);
	lck_mtx_lock(&ufs_lock);
	if (!parse_args() || !take_controller()) {
		lck_mtx_unlock(&ufs_lock);
		printf("mtkufs: test: FAILED before the first read (%s, %#x)\n", ufs_failed ? ufs_failed : "?", ufs_failed_status);
		return;
	}
	buf = kalloc_data(BOUNCE_SIZE, Z_WAITOK);
	if (buf == NULL) {
		lck_mtx_unlock(&ufs_lock);
		return;
	}
	len = part_blocks * LU_BS < 4 * 1024 * 1024 ? part_blocks * LU_BS : 4 * 1024 * 1024;
	t0 = mach_absolute_time();
	for (at = 0; at < len; at += BOUNCE_SIZE) {
		uint64_t n = len - at < BOUNCE_SIZE ? len - at : BOUNCE_SIZE;

		if (!read_bounced(at, buf, 0, n)) {
			break;
		}
		if (at == 0) {
			printf("mtkufs: test: block %llu of LUN %u starts %02x %02x %02x %02x %02x %02x %02x %02x \"%c%c%c%c%c%c%c%c\"\n",
			    part_lba, ufs_lun, buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7],
			    buf[0] >= 0x20 && buf[0] < 0x7f ? buf[0] : '.', buf[1] >= 0x20 && buf[1] < 0x7f ? buf[1] : '.',
			    buf[2] >= 0x20 && buf[2] < 0x7f ? buf[2] : '.', buf[3] >= 0x20 && buf[3] < 0x7f ? buf[3] : '.',
			    buf[4] >= 0x20 && buf[4] < 0x7f ? buf[4] : '.', buf[5] >= 0x20 && buf[5] < 0x7f ? buf[5] : '.',
			    buf[6] >= 0x20 && buf[6] < 0x7f ? buf[6] : '.', buf[7] >= 0x20 && buf[7] < 0x7f ? buf[7] : '.');
		}
		for (uint64_t i = 0; i < n; i += 8) {
			uint64_t w;

			memcpy(&w, buf + i, 8);
			sum = (sum ^ w) * 0x100000001b3ULL;
		}
	}
	if (at < len) {
		printf("mtkufs: test: FAILED at byte %llu: %s (%#x)\n", at, ufs_failed ? ufs_failed : "?", ufs_failed_status);
	} else {
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("mtkufs: test: %llu KB in %llu ms, sum %016llx (the loader's must be the same)\n", len >> 10,
		    ns / 1000000, sum);
		test_direct(len, sum);
		len = part_blocks * LU_BS < 16 * 1024 * 1024 ? part_blocks * LU_BS : 16 * 1024 * 1024;
		t0 = mach_absolute_time();
		for (at = 0; at < len && read_bounced(at, buf, 0, BOUNCE_SIZE); at += BOUNCE_SIZE) {
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		printf("mtkufs: test: bounced: %llu MB in %llu ms%s; %llu reads (%llu direct), %llu errors\n", at >> 20,
		    ns / 1000000, at < len ? " (stopped by an error)" : "", ufs_reads, ufs_direct_reads, ufs_errors);
		if (mode >= 2) {
			test_big();
		}
	}
	kfree_data(buf, BOUNCE_SIZE);
	lck_mtx_unlock(&ufs_lock);
}

void
redwood_mtkufs_devfs(void)
{
	if (ufs_bmajor < 0 || ufs_cmajor < 0) {
		return;
	}
	if (devfs_make_node(makedev(ufs_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "ufs0") == NULL ||
	    devfs_make_node(makedev(ufs_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rufs0") == NULL) {
		printf("mtkufs: no /dev nodes\n");
	}
}

/* sysctl kern.redwood.ufs: how much it has read, and its errors */
static int
ufs_sysctl SYSCTL_HANDLER_ARGS
{
#pragma unused(oidp, arg1, arg2)
	char line[160];

	snprintf(line, sizeof(line), "ufs: %llu reads (%llu direct), %llu MB, %llu errors%s%s", ufs_reads, ufs_direct_reads,
	    (ufs_read_blocks * LU_BS) >> 20, ufs_errors, ufs_failed ? "; last: " : "", ufs_failed ? ufs_failed : "");
	return SYSCTL_OUT(req, line, strlen(line) + 1);
}

SYSCTL_DECL(_kern_redwood);
SYSCTL_PROC(_kern_redwood, OID_AUTO, ufs, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, ufs_sysctl, "A",
    "the UFS's reads so far");
