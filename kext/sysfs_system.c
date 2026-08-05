/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_system.c
 *
 * Content for /sys/devices/system - Linux's "system" pseudo-bus. Unlike the rest
 * of /sys/devices (which mirrors the IOKit registry), this hierarchy is
 * synthetic on Linux too: it describes CPUs, memory and NUMA nodes rather than
 * discovered hardware. The tree itself is built in sysfs_structure.c; this file
 * supplies the data its files read.
 */
#include <sys/uio.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>

/*
 * A counter that is always zero, rendered the way Linux renders sysfs integer
 * attributes: the decimal value followed by a newline.
 *
 * This backs the per-node huge-page counters
 * (/sys/devices/system/node/node0/hugepages/hugepages-2048kB/{nr,free,surplus}_hugepages).
 * macOS has no hugetlb pool: large pages exist, but they are managed
 * transparently by the VM system and there is no persistent pool to size,
 * reserve or overcommit, so nothing can be allocated to a node. Reporting 0 is
 * both truthful and exactly what a Linux host with no huge pages configured
 * reports, which is what callers probing these files are prepared to handle.
 */
int
sysfs_do_zero_count(sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char zero[] = "0\n";

    (void)snp;
    return sysfs_copy_data(zero, (int)(sizeof(zero) - 1), uio);
}
