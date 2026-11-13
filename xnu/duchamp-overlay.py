#!/usr/bin/env python3
"""Put duchamp's kernel additions into a HakiOS XNU tree (branch mtk, after its xnu/apply.sh).

    python3 xnu/duchamp-overlay.py XNU_SRC

Copies xnu/files/ over the tree (bsd/dev/redwood_mtkufs.c: the UFS block device; bsd/sys/redwood_mtkufs.h) and hooks
it in where the tree's own eMMC driver is hooked: bsd/conf/files, bsd/kern/bsd_init.c (/dev/ufs0) and
iokit/bsddev/IOKitBSDInit.cpp (boot-args ufs_root=1 and ufs_test=1); and in osfmk/arm64/psci_smp.c, core 0 kept by
mtkloader for the kernel (cpu_park=); in osfmk/arm/io_map.c, the early mappings' next address moved on by all that
was mapped (the framebuffer's last page went to the GIC); and bsd/dev/redwood_mtktouch.c, the touch screen (boot-arg
mtktouch=, which mtkloader gives with XNU_TOUCH=1), started where the tree's other touch drivers are; and bsd/dev/redwood_mtkgpu.c, the GPU (boot-arg
mtkgpu=1, which mtkloader gives with XNU_GPU=1), started after it. Each change is made once: running it again on the same tree
changes nothing but the copied files. A tree that does not look as expected is left alone with an error.
"""
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

UFS_BLOCK = '''//
//	duchamp (POCO X6 Pro, MT6897): the system image on the UFS, read as it is needed (bsd/dev/redwood_mtkufs.c).
//	ufs_root=1 beside rd=md0, as emmc_root=1 above; mtkloader brings the controller up and names the partition
//	(ufs_lun=, ufs_lba=, ufs_blocks=). ufs_test=1 only reads it and says what it read; md0 stays the root.
//	In a block of its own: the gotos above may not jump over an initialised variable.
//
	{
		uint32_t ufsTest = 0, ufsRoot = 0;
		if (PE_parse_boot_argn("ufs_test", &ufsTest, sizeof(ufsTest)) && ufsTest) {
			redwood_mtkufs_test();
		}
		if (PE_parse_boot_argn("ufs_root", &ufsRoot, sizeof(ufsRoot)) && ufsRoot) {
			OSSafeReleaseNULL(matching);
			if (redwood_mtkufs_root(root) != 0) {
				panic("IOFindBSDRoot: ufs_root=1, but the system image on the UFS could not be read");
			}
			strlcpy(rootName, "ufs0", rootNameSize);
			IOLog("BSD root: %s, major %d, minor %d\\n", rootName, major(*root), minor(*root));
			*oflags = 0;
			goto iofrootx;
		}
	}

'''

PARK_FUNC = '''/*
 * duchamp (POCO X6 Pro, MT6897): a core the boot loader kept for the kernel. mtkloader starts the kernel on core 7 and
 * LK left the loader on core 0; switched off with CPU_OFF, core 0 never came back when this file asked for it with
 * CPU_ON (the firmware said yes, the core never arrived). So with cpus=8 mtkloader keeps it waiting, MMU and caches
 * off, on a mailbox whose physical address is cpu_park= on the command line: { "PARK", its MPIDR, the entry (written
 * here), -, its state: 1 waiting, 2 gone }. Given the reset vector, it comes in as CPU_ON would have brought it.
 */
#define PSCI_PARK_MAGIC         0x4b524150u     /* "PARK" */

static bool
psci_smp_parked(uint32_t phys_id, uint64_t entry)
{
	static volatile uint64_t *box;
	static bool looked;
	uint64_t pa = 0;

	if (!looked) {
		looked = true;
		if (PE_parse_boot_argn("cpu_park", &pa, sizeof(pa)) && pa != 0 && (pa & 63) == 0) {
			/* uncached, as the core reads it with its MMU off; outside the kernel's memory, so mapped only here */
			vm_offset_t va = ml_io_map((vm_offset_t)(pa & ~(uint64_t)PAGE_MASK), PAGE_SIZE);

			if (va != 0) {
				box = (volatile uint64_t *)(va + (vm_offset_t)(pa & PAGE_MASK));
			}
		}
	}
	if (box == NULL || (uint32_t)box[0] != PSCI_PARK_MAGIC || (box[1] & 0xffff) != (phys_id & 0xffff) || box[4] != 1) {
		return false;
	}
	box[2] = entry;
	__builtin_arm_dsb(DSB_SY);
	__builtin_arm_sev();
	for (int ms = 0; ms < 100 && box[4] != 2; ms++) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	printf("psci: MPIDR 0x%x was kept by the boot loader (cpu_park): %s\\n", phys_id,
	    box[4] == 2 ? "sent to the reset vector" : "it has not answered in 100 ms");
	return true;
}

'''

# Before the VM is up, io_map() maps round_page(size + the address's offset in its page) and moves the next free address
# on by round_page(size) only: a framebuffer not on a 16 KB boundary (LK's, 0x3000 into one) lost its last page to the
# next mapping, the GIC's distributor, and the panel's bottom row wrote into GICD_CTLR (run 20, docs/porting-mt6897.md).
IO_MAP_FIX = '''\t\t/* duchamp: what is mapped just below, offset in the page and all: with round_page(size) the next mapping (the
\t\t * GIC's distributor, after the framebuffer LK left 0x3000 into a page) took the last page of this one */
\t\tvirtual_space_start += alloc_size;
'''

# (file, anchor, text, where): the text goes after (or before) the anchor's line, or takes the anchor's place, once
EDITS = [
    ('iokit/conf/files', 'iokit/Kernel/RedwoodMali.cpp\t\t\t\toptional iokitcpp\n',
     'iokit/Kernel/hakios_Panthor.cpp\t\t\toptional iokitcpp\n', 'after'),
    ('bsd/conf/files', 'bsd/dev/redwood_emmc.c\tstandard\n', 'bsd/dev/redwood_mtkufs.c\tstandard\n', 'after'),
    ('bsd/kern/bsd_init.c', '#include <sys/redwood_emmc.h>\n', '#include <sys/redwood_mtkufs.h>\n', 'after'),
    ('bsd/kern/bsd_init.c', '\t\tredwood_emmc_devfs();',
     '\t\tredwood_mtkufs_devfs();         /* /dev/ufs0, duchamp\'s system image (ufs_root=1) */\n', 'after'),
    ('iokit/bsddev/IOKitBSDInit.cpp', 'extern int redwood_emmc_root(dev_t *root);\n',
     'extern int redwood_mtkufs_root(dev_t *root);\nextern void redwood_mtkufs_test(void);\n', 'after'),
    ('iokit/bsddev/IOKitBSDInit.cpp', "//\n//\tNow check if we are trying to root on a memory device", UFS_BLOCK, 'before'),
    ('osfmk/arm64/psci_smp.c', '/*\n * Start a registered processor: the platform\'s half of cpu_start().', PARK_FUNC, 'before'),
    ('osfmk/arm64/psci_smp.c', '\tstatus = psci_call(PSCI_CPU_ON, phys_id, entry, 0);\n',
     '\tstatus = psci_smp_parked(phys_id, entry) ? 0 : psci_call(PSCI_CPU_ON, phys_id, entry, 0);\n', 'replace'),
    ('osfmk/arm/io_map.c', '\t\tvirtual_space_start += round_page(size);\n', IO_MAP_FIX, 'replace'),
    ('bsd/conf/files', 'bsd/dev/redwood_mtkufs.c\tstandard\n', 'bsd/dev/redwood_mtktouch.c\tstandard\n', 'after'),
    ('bsd/sys/redwood_input.h', 'void redwood_tcm_init(void);',
     'void redwood_mtktouch_init(void);       /* boot-arg mtktouch=1/2 (mtkloader): duchamp\'s Goodix or FocalTech touch screen */\n',
     'after'),
    ('bsd/dev/redwood_input.c', '\tredwood_tcm_init();\n', '\tredwood_mtktouch_init();\n', 'after'),
    ('bsd/conf/files', 'bsd/dev/redwood_mtktouch.c\tstandard\n', 'bsd/dev/redwood_mtkgpu.c\tstandard\n', 'after'),
    ('bsd/sys/redwood_input.h', 'void redwood_mtktouch_init(void);',
     'void redwood_mtkgpu_init(void);         /* boot-arg mtkgpu=1 (mtkloader XNU_GPU=1): duchamp\'s Mali-G615, its MCU and a first command stream */\n',
     'after'),
    ('bsd/dev/redwood_input.c', '\tredwood_mtktouch_init();\n', '\tredwood_mtkgpu_init();\n', 'after'),
]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    tree = sys.argv[1]
    if not os.path.isfile(os.path.join(tree, 'bsd/dev/redwood_emmc.c')):
        sys.exit(f'duchamp-overlay: {tree} is not a HakiOS XNU tree of branch mtk (no bsd/dev/redwood_emmc.c)')
    pending = {}
    for rel, anchor, text, where in EDITS:
        path = os.path.join(tree, rel)
        if path in pending:
            s = pending[path]
        else:
            with open(path) as f:
                s = f.read()
        if text in s:
            continue
        at = s.find(anchor)
        if at < 0 or s.find(anchor, at + 1) >= 0:
            sys.exit(f'duchamp-overlay: {rel}: the place for the change is not there once ({anchor.strip()!r})')
        if where == 'replace':
            s = s[:at] + text + s[at + len(anchor):]
        elif where == 'after':
            end = s.index('\n', at + len(anchor) - 1) + 1 if not anchor.endswith('\n') else at + len(anchor)
            s = s[:end] + text + s[end:]
        else:
            s = s[:at] + text + s[at:]
        pending[path] = s
    # All anchors validated; mismatched trees receive no copies or partial edits.
    files = os.path.join(HERE, 'files')
    for root, _, names in os.walk(files):
        for name in names:
            src = os.path.join(root, name)
            dst = os.path.join(tree, os.path.relpath(src, files))
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copyfile(src, dst)
            print('duchamp-overlay: copied', os.path.relpath(src, files))
    for path, s in pending.items():
        with open(path, 'w') as f:
            f.write(s)
        print('duchamp-overlay: changed', os.path.relpath(path, tree))


if __name__ == '__main__':
    main()
