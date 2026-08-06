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
 *
 * macOS is not NUMA. There is one node (node0) owning every CPU and all memory,
 * which is exactly how Linux presents a non-NUMA machine, so the single-node
 * answers here are the truthful ones rather than placeholders.
 */
#include <libkern/libkern.h>

#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>

/*
 * sysctlbyname() is exported to kexts (it resolves at load like any other KPI),
 * but <sys/sysctl.h> only declares it in its !KERNEL branch, so a kernel
 * translation unit that includes the header still does not see a prototype.
 * Declare it here rather than reaching for a private header.
 */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        void *newp, size_t newlen);

/*
 * Largest rendering any file here produces. The CPU map is the widest: one hex
 * nibble per 4 CPUs plus comma separators.
 */
#define SYSFS_SYSTEM_BUFMAX 512

/*
 * Number of logical CPUs. hw.logicalcpu_max is the count the machine can ever
 * have online, which is what the "possible"/"present" masks must describe;
 * macOS never hot-unplugs a core, so "online" is the same set.
 */
static uint32_t
sysfs_ncpu(void)
{
    int ncpu = 0;
    size_t len = sizeof(ncpu);

    if (sysctlbyname("hw.logicalcpu_max", &ncpu, &len, NULL, 0) != 0 || ncpu <= 0) {
        ncpu = 1;
    }
    return (uint32_t)ncpu;
}

/*
 * Total physical memory in bytes.
 */
static uint64_t
sysfs_memsize(void)
{
    uint64_t bytes = 0;
    size_t len = sizeof(bytes);

    if (sysctlbyname("hw.memsize", &bytes, &len, NULL, 0) != 0) {
        bytes = 0;
    }
    return bytes;
}

/*
 * A counter that is always zero, rendered the way Linux renders sysfs integer
 * attributes: the decimal value followed by a newline.
 *
 * This backs the per-node huge-page counters. macOS has no hugetlb pool: large
 * pages exist, but they are managed transparently by the VM system and there is
 * no persistent pool to size, reserve or overcommit, so nothing can be allocated
 * to a node. Reporting 0 is both truthful and exactly what a Linux host with no
 * huge pages configured reports.
 */
int
sysfs_do_zero_count(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char zero[] = "0\n";

    return sysfs_copy_data(zero, (int)(sizeof(zero) - 1), uio);
}

/*
 * A CPU list in Linux's "cpulist" form: a comma-separated list of ranges, e.g.
 * "0-9". Every CPU belongs to the single node and is always online, so this is
 * one contiguous range (or just "0" on a uniprocessor).
 *
 * Backs node0/cpulist and cpu/{online,possible,present}.
 */
int
sysfs_do_cpulist(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    char buf[SYSFS_SYSTEM_BUFMAX];
    uint32_t ncpu = sysfs_ncpu();
    int len;

    if (ncpu <= 1) {
        len = snprintf(buf, sizeof(buf), "0\n");
    } else {
        len = snprintf(buf, sizeof(buf), "0-%u\n", ncpu - 1);
    }
    return sysfs_copy_data(buf, len, uio);
}

/*
 * The same set as a Linux "cpumap": a big-endian hex bitmask, 32 bits per
 * comma-separated group, least-significant group last. All CPUs are set.
 */
int
sysfs_do_cpumap(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    char buf[SYSFS_SYSTEM_BUFMAX];
    uint32_t ncpu = sysfs_ncpu();
    uint32_t groups = (ncpu + 31) / 32;
    int off = 0;

    /*
     * Emit most-significant group first. Every group is all-ones except the
     * most significant one, which is masked to the CPUs that actually exist.
     */
    for (uint32_t g = groups; g > 0 && off < (int)sizeof(buf); g--) {
        uint32_t bits_in_group = (g == groups) ? (ncpu - (groups - 1) * 32) : 32;
        uint32_t mask = (bits_in_group >= 32)
            ? 0xffffffffu
            : ((1u << bits_in_group) - 1u);

        off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                        (g == groups) ? "%08x" : ",%08x", mask);
    }
    off += snprintf(buf + off, sizeof(buf) - (size_t)off, "\n");

    return sysfs_copy_data(buf, off, uio);
}

/*
 * node0/meminfo, in Linux's per-node format. Only the totals macOS can state
 * exactly are reported with real values; the breakdown fields Linux derives from
 * its own zone accounting have no faithful XNU equivalent and are 0, as they
 * would be on a node with nothing of that kind allocated.
 */
int
sysfs_do_node_meminfo(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    char buf[SYSFS_SYSTEM_BUFMAX];
    uint64_t total_kb = sysfs_memsize() / 1024;
    int off = 0;

    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "Node 0 MemTotal:       %8llu kB\n", total_kb);
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "Node 0 MemUsed:        %8llu kB\n", (uint64_t)0);
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "Node 0 MemFree:        %8llu kB\n", (uint64_t)0);
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "Node 0 HugePages_Total: %7llu\n", (uint64_t)0);
    off += snprintf(buf + off, sizeof(buf) - (size_t)off,
                    "Node 0 HugePages_Free:  %7llu\n", (uint64_t)0);

    return sysfs_copy_data(buf, off, uio);
}

/*
 * node0/distance. The NUMA distance matrix has one entry per node; with a single
 * node that is just the self-distance, which Linux defines as 10.
 */
int
sysfs_do_node_distance(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char self[] = "10\n";

    return sysfs_copy_data(self, (int)(sizeof(self) - 1), uio);
}

/*
 * node0/numastat. Every allocation is local on a single-node machine, so the
 * miss/foreign/other counters are 0 by construction rather than by omission.
 * numa_hit is not tracked by XNU, so it is reported as 0 too.
 */
int
sysfs_do_node_numastat(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char stat[] =
        "numa_hit 0\n"
        "numa_miss 0\n"
        "numa_foreign 0\n"
        "interleave_hit 0\n"
        "local_node 0\n"
        "other_node 0\n";

    return sysfs_copy_data(stat, (int)(sizeof(stat) - 1), uio);
}

/*
 * node/has_cpu, node/has_memory, node/online, node/possible - the node-set
 * files. One node, which has both CPUs and memory, so all four read "0".
 */
int
sysfs_do_node_list(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    static const char node0[] = "0\n";

    return sysfs_copy_data(node0, (int)(sizeof(node0) - 1), uio);
}

/*
 * cpu/kernel_max - the largest CPU index the kernel could ever use. Linux
 * reports NR_CPUS-1; the analogous bound here is the maximum logical CPU count.
 */
int
sysfs_do_cpu_kernel_max(__unused sfsnode_t *snp, uio_t uio, __unused vfs_context_t ctx)
{
    char buf[SYSFS_SYSTEM_BUFMAX];
    int len = snprintf(buf, sizeof(buf), "%u\n", sysfs_ncpu() - 1);

    return sysfs_copy_data(buf, len, uio);
}
