/*
 * The POCO X6 Pro's UFS (duchamp, MT6897): the system image's partition as a block device, read as the kernel asks
 * for it. See bsd/dev/redwood_mtkufs.c.
 */
#ifndef _SYS_REDWOOD_MTKUFS_H_
#define _SYS_REDWOOD_MTKUFS_H_

#include <sys/types.h>

__BEGIN_DECLS
int redwood_mtkufs_root(dev_t *root);      /* ufs_root=1: the device, set up on the first call; 0 or an errno */
void redwood_mtkufs_test(void);            /* ufs_test=1: the partition read and summed, not mounted; =2: 12 GB of it */
void redwood_mtkufs_devfs(void);           /* /dev/ufs0, /dev/rufs0 once devfs is up */
__END_DECLS

#endif /* _SYS_REDWOOD_MTKUFS_H_ */
