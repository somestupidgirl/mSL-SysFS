/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_structure.c
 *
 * Definition and management of the file system layout. The layout is defined by
 * a tree of sfssnode_t objects, starting with the root of the file system. The
 * structure is created in the sysfs_structure_init() function and is used while
 * servicing VNOP_LOOKUP and VNOP_READDIR. To add new file system nodes, add the
 * corresponding entries in sysfs_structure_init() and make any necessary changes
 * in the sysfs_vnop_lookup() and sysfs_vnop_readdir() functions. When adding
 * files, it's also necessary to add functions that return the file's data and
 * its size, unless the size is fixed, and link to them from the sfssnode_t.
 *
 * This scaffold pass builds only the fixed Linux /sys top-level skeleton: the
 * root and the standard top-level directories, all empty. The dynamic,
 * IORegistry-backed content (/sys/devices and the class/bus/block/dev symlink
 * views into it, /sys/module, /sys/kernel, ...) is a later pass; it will hang
 * dynamic-expansion (SSN_FLAG_DYNAMIC) marker nodes under these directories,
 * exactly as procfs hangs its per-process marker nodes.
 */
#include <string.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <mach/boolean.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/sysfs_iokit.h>

#pragma mark -
#pragma mark Function Prototypes

STATIC sfssnode_t *add_node(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, size_t size,
                        sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn);

STATIC sfssnode_t *add_directory(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, boolean_t raw,
                        sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn);

STATIC sfssnode_t *add_file(sfssnode_t *parent, const char *name, sfsbaseid_t node_id, uint16_t flags, size_t size,
                        sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn);

STATIC void release_node(sfssnode_t *root);

/*
 * Depth bound for release_node()'s explicit node stack (it is iterative rather
 * than recursive so a deep tree cannot overflow the kernel stack). The skeleton
 * tree is a few dozen nodes, so this is generous.
 */
#define SYSFS_RELEASE_STACK_MAX 256

/*
 * Upper bound on generated /sys/devices/system/cpu/cpuN directories. The static
 * tree is torn down through release_node()'s fixed-size stack, so the number of
 * nodes the CPU loop can add has to stay well inside that bound.
 */
#define SYSFS_MAX_CPU_NODES 64

/*
 * Next node id. No need to lock this value because access is guaranteed to be
 * single-threaded. Start at 2 because the root node is always 1.
 */
STATIC uint16_t next_node_id = SYSFS_ROOT_NODE_BASE_ID + 1;

/*
 * The root of the file system structure.
 */
static sfssnode_t *root_node;

#pragma mark -
#pragma mark Externally Visible Functions

/*
 * Gets the root node of the file system structure.
 */
sfssnode_t *
sysfs_structure_root_node(void)
{
    return root_node;
}

/*
 * Initializes the sysfs structures. Should only be called while mounting a file
 * system. Given that restriction, we do not need to lock access to the structure
 * data when deciding whether to create it.
 *
 * NOTE: it is essential that any entries that expand to dynamic content be the
 * last in their parent's child list (as in procfs). None do yet - the scaffold
 * tree is entirely static - but the later IORegistry passes must preserve this.
 */
void
sysfs_structure_init(void)
{
    /*
     * Only do this on first mount.
     */
    if (root_node == NULL) {
        /*
         * The root directory of the file system (/sys). This happens to be the
         * only node that has the same node id on all instances of this file
         * system.
         */
        root_node = add_directory(NULL, "/", SFSroot, SYSFS_ROOT_NODE_BASE_ID, 0, 0, NULL, NULL);

        /*
         * The fixed Linux /sys top-level directories, created empty. Each is a
         * mount point for a later pass:
         *
         *   block/       symlink view of block devices (into devices/)
         *   bus/         per-bus devices/ + drivers/ (IOKit provider families)
         *   class/       device-class groupings (net, tty, power_supply, ...)
         *   dev/         char/ + block/ major:minor symlinks
         *   devices/     the device model itself (mirror of the IORegistry)
         *   firmware/    device tree / ACPI / DMI
         *   fs/          filesystem-specific tunables
         *   hypervisor/  hypervisor interface (empty off a hypervisor)
         *   kernel/      kernel tunables and info (sysctl-backed)
         *   module/      loaded kernel modules (kexts)
         *   power/       system power-management state
         */
        (void)add_directory(root_node, "block",      SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "bus",        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        /*
         * /sys/class/<class>/<device> - devices grouped by class, each entry a
         * symlink into /sys/devices, as on Linux. Every class directory is
         * marked dynamic and carries its class in ssn_instance; its single
         * SFSlink child is the shared marker the members are presented through
         * (the device itself is identified per-vnode by its registry id).
         */
        sfssnode_t *class_dir = add_directory(root_node, "class",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        static const struct {
            const char *name;
            uint32_t    id;
        } sysfs_classes[] = {
            { "net",          SYSFS_CLASS_NET   },
            { "block",        SYSFS_CLASS_BLOCK },
            { "tty",          SYSFS_CLASS_TTY   },
            { "power_supply", SYSFS_CLASS_POWER },
        };
        for (size_t ci = 0; ci < sizeof(sysfs_classes) / sizeof(sysfs_classes[0]); ci++) {
            sfssnode_t *cd = add_directory(class_dir, sysfs_classes[ci].name,
                            SFSdir, next_node_id++, SSN_FLAG_DYNAMIC, 0, NULL, NULL);
            cd->ssn_instance = sysfs_classes[ci].id;
            /*
             * The marker every member of this class is presented through. It is
             * never listed itself - the class readdir emits members instead.
             */
            (void)add_node(cd, "__member__", SFSlink, next_node_id++,
                            SSN_FLAG_DYNAMIC, 0, NULL, NULL);
        }

        /*
         * dev/ holds the char/ and block/ major:minor symlink views.
         */
        sfssnode_t *dev_dir = add_directory(root_node, "dev", SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(dev_dir, "char",  SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(dev_dir, "block", SFSdir, next_node_id++, 0, 0, NULL, NULL);

        /*
         * /sys/devices - the IORegistry mirror. A single SFSdevice node backs the
         * whole subtree; which registry entry a given directory stands for is
         * carried per-vnode in the node id's regid (0 == the registry root ==
         * this directory itself). Added with add_node (not add_directory) so it
         * has no static "."/".." children - the dynamic readdir emits those
         * itself (see sysfs_devices_readdir), exactly as procfs does for /proc/sys.
         */
        sfssnode_t *devices_node =
            add_node(root_node, "devices", SFSdevice, next_node_id++, SSN_FLAG_DYNAMIC, 0, NULL, NULL);

        /*
         * /sys/devices/system - Linux's "system" pseudo-bus. Unlike the rest of
         * /sys/devices this is NOT part of the IOKit mirror: on Linux it is a
         * synthetic hierarchy describing CPUs, memory and NUMA nodes, so it is
         * built here as ordinary static structure nodes. The SFSdevice lookup and
         * readdir paths consult these static children alongside the registry
         * ones, but only at the /sys/devices root (registry id 0).
         *
         * macOS is not NUMA, so there is exactly one node, node0.
         */
        sfssnode_t *system_dir = add_directory(devices_node, "system",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *node_dir   = add_directory(system_dir, "node",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *node0_dir  = add_directory(node_dir, "node0",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);

        /*
         * node0/hugepages/hugepages-<size>kB/. The huge-page size is 2MB on both
         * Apple Silicon and Intel Macs (the architectural large-page size; the
         * base page is 16KB on arm64 and 4KB on x86_64), so the directory is
         * named for 2048kB on either.
         *
         * macOS has no hugetlb pool to size, reserve or overcommit - large pages
         * are managed transparently by the VM - so all three counters read 0,
         * exactly as on a Linux host where no huge pages have been allocated.
         * Per-node hugepages directories expose these three files only; the
         * resv_/nr_overcommit_ counters are global (/sys/kernel/mm/hugepages).
         */
        sfssnode_t *hugepages_dir = add_directory(node0_dir, "hugepages",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *hp_2m_dir = add_directory(hugepages_dir, "hugepages-2048kB",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);

        add_file(hp_2m_dir, "nr_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(hp_2m_dir, "free_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(hp_2m_dir, "surplus_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);

        /*
         * The rest of node0's standard attributes. Everything belongs to the one
         * node, so the CPU set is every CPU and the distance matrix is just the
         * self-distance.
         */
        add_file(node0_dir, "cpulist",  next_node_id++, 0, 0, NULL, sysfs_do_cpulist);
        add_file(node0_dir, "cpumap",   next_node_id++, 0, 0, NULL, sysfs_do_cpumap);
        add_file(node0_dir, "meminfo",  next_node_id++, 0, 0, NULL, sysfs_do_node_meminfo);
        add_file(node0_dir, "distance", next_node_id++, 0, 0, NULL, sysfs_do_node_distance);
        add_file(node0_dir, "numastat", next_node_id++, 0, 0, NULL, sysfs_do_node_numastat);

        /*
         * The node-set files that live beside the nodeN directories. One node,
         * which has both CPUs and memory, so each of these reads "0".
         */
        add_file(node_dir, "online",     next_node_id++, 0, 0, NULL, sysfs_do_node_list);
        add_file(node_dir, "possible",   next_node_id++, 0, 0, NULL, sysfs_do_node_list);
        add_file(node_dir, "has_cpu",    next_node_id++, 0, 0, NULL, sysfs_do_node_list);
        add_file(node_dir, "has_memory", next_node_id++, 0, 0, NULL, sysfs_do_node_list);

        /*
         * /sys/devices/system/cpu - the CPU topology sets. macOS never
         * hot-unplugs a core, so online == possible == present == every CPU.
         */
        sfssnode_t *cpu_dir = add_directory(system_dir, "cpu",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        add_file(cpu_dir, "online",     next_node_id++, 0, 0, NULL, sysfs_do_cpulist);
        add_file(cpu_dir, "possible",   next_node_id++, 0, 0, NULL, sysfs_do_cpulist);
        add_file(cpu_dir, "present",    next_node_id++, 0, 0, NULL, sysfs_do_cpulist);
        add_file(cpu_dir, "offline",    next_node_id++, 0, 0, NULL, sysfs_do_zero_count);
        add_file(cpu_dir, "kernel_max", next_node_id++, 0, 0, NULL, sysfs_do_cpu_kernel_max);

        /*
         * cpu/cpuN/ with its topology. These are created statically, one per
         * logical CPU, because macOS never hot-plugs a core: the count is fixed
         * for the life of the boot, so there is nothing to expand dynamically.
         * Each node carries its CPU number in ssn_instance, which is how one
         * handler serves every cpuN.
         */
        uint32_t ncpu = sysfs_cpu_count();
        for (uint32_t c = 0; c < ncpu && c < SYSFS_MAX_CPU_NODES; c++) {
            char cpuname[MAX_STRUCT_NODE_NAME_LEN];
            snprintf(cpuname, sizeof(cpuname), "cpu%u", c);

            sfssnode_t *cpuN = add_directory(cpu_dir, cpuname,
                            SFSdir, next_node_id++, 0, 0, NULL, NULL);
            cpuN->ssn_instance = c;

            sfssnode_t *f = add_file(cpuN, "online", next_node_id++, 0, 0,
                            NULL, sysfs_do_cpu_online);
            f->ssn_instance = c;

            sfssnode_t *topo = add_directory(cpuN, "topology",
                            SFSdir, next_node_id++, 0, 0, NULL, NULL);
            topo->ssn_instance = c;

            f = add_file(topo, "core_id", next_node_id++, 0, 0,
                            NULL, sysfs_do_cpu_core_id);
            f->ssn_instance = c;
            f = add_file(topo, "physical_package_id", next_node_id++, 0, 0,
                            NULL, sysfs_do_cpu_package_id);
            f->ssn_instance = c;
            f = add_file(topo, "thread_siblings_list", next_node_id++, 0, 0,
                            NULL, sysfs_do_cpu_thread_siblings);
            f->ssn_instance = c;
            f = add_file(topo, "core_siblings_list", next_node_id++, 0, 0,
                            NULL, sysfs_do_cpu_core_siblings);
            f->ssn_instance = c;
        }

        (void)add_directory(root_node, "firmware",   SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "fs",         SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "hypervisor", SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *kernel_dir =
            add_directory(root_node, "kernel",     SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "module",     SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "power",      SFSdir, next_node_id++, 0, 0, NULL, NULL);

        /*
         * /sys/kernel/mm/hugepages/hugepages-2048kB/ - the system-wide huge-page
         * pool, the counterpart to the per-node counters above. Linux exposes two
         * extra knobs here that have no per-node equivalent (reservations and the
         * overcommit limit); like the rest they are 0, because macOS has no
         * hugetlb pool to reserve from or overcommit.
         */
        sfssnode_t *mm_dir = add_directory(kernel_dir, "mm",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *mm_hp_dir = add_directory(mm_dir, "hugepages",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);
        sfssnode_t *mm_hp_2m = add_directory(mm_hp_dir, "hugepages-2048kB",
                        SFSdir, next_node_id++, 0, 0, NULL, NULL);

        add_file(mm_hp_2m, "nr_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(mm_hp_2m, "free_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(mm_hp_2m, "surplus_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(mm_hp_2m, "resv_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
        add_file(mm_hp_2m, "nr_overcommit_hugepages", next_node_id++, 0,
                        SYSFS_ZERO_COUNT_LEN, NULL, sysfs_do_zero_count);
    }
}

/*
 * Frees the memory for the sysfs structures. Should only be called while
 * unmounting the last instance of the file system. Given that restriction, we do
 * not need to lock access to the structure data when freeing it.
 */
void
sysfs_structure_free(void)
{
    if (root_node != NULL) {
        /*
         * Release the root node. This recursively releases all descendent nodes.
         */
        release_node(root_node);
        root_node = NULL;
    }
}

#pragma mark -
#pragma mark Creation of Structure Nodes

/*
 * Adds a node to the file system structure. This is a low-level function that is
 * called by add_directory() (and, in later passes, add_file()). It should not be
 * called directly.
 */
STATIC sfssnode_t *
add_node(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, size_t size,
            sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn)
{
    sfssnode_t *node = (sfssnode_t *)OSMalloc(sizeof(sfssnode_t), sysfs_osmalloc_tag);
    if (node == NULL) {
        panic("Unable to allocate memory for sfssnode_t");
    }

    bzero(node, sizeof(sfssnode_t));
    node->ssn_node_type = type;
    /*
     * A truncated name is not a cosmetic problem: two names sharing a prefix
     * collapse to the same entry, so the directory lists duplicates and neither
     * node can be looked up. Complain rather than truncate silently.
     */
    if (strlcpy(node->ssn_name, name, sizeof(node->ssn_name)) >= sizeof(node->ssn_name)) {
        LOG_ERR("node name '%s' truncated to '%s'; raise MAX_STRUCT_NODE_NAME_LEN",
                name, node->ssn_name);
    }
    node->ssn_base_node_id = node_id;
    node->ssn_flags = flags;
    node->ssn_parent = parent;
    node->ssn_node_size = size;
    node->ssn_getsize_fn = node_size_fn;
    node->ssn_read_data_fn = node_read_data_fn;

    TAILQ_INIT(&node->ssn_children);
    if (parent != NULL) {
        /*
         * Add this node to the tail of its parent's child list.
         */
        TAILQ_INSERT_TAIL(&parent->ssn_children, node, ssn_next);

        /*
         * Propagate the SSN_FLAG_DYNAMIC flag downward, so a subtree's nature can
         * be recognised from any descendant's flags alone.
         */
        node->ssn_flags |= (parent->ssn_flags & SSN_FLAG_DYNAMIC);
    }
    return node;
}

/*
 * Adds a directory node to the file system structure. Since all directories must
 * have "." and ".." entries, these are added here by a recursive call with the
 * raw argument set to 1 to avoid infinite recursion.
 */
STATIC sfssnode_t *
add_directory(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, boolean_t raw,
                sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn)
{
    /*
     * Add the directory node.
     */
    sfssnode_t *snode = add_node(parent, name, type, node_id, flags, 0, node_size_fn, node_read_data_fn);

    /*
     * Add the "." and ".." directory entries, preserving the dynamic flag. The
     * "raw" argument is used to stop this being a recursive process.
     */
    if (!raw) {
        add_directory(snode, ".",  SFSdirthis,   next_node_id++, flags & SSN_FLAG_DYNAMIC, 1, NULL, NULL);
        add_directory(snode, "..", SFSdirparent, next_node_id++, flags & SSN_FLAG_DYNAMIC, 1, NULL, NULL);
    }
    return snode;
}

/*
 * Adds a file to the file system structure. Files are always leaf elements
 * (although that is not checked).
 */
STATIC sfssnode_t *
add_file(sfssnode_t *parent, const char *name, sfsbaseid_t node_id, uint16_t flags, size_t size,
            sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn)
{
    return add_node(parent, name, SFSfile, node_id, flags, size, node_size_fn, node_read_data_fn);
}

#pragma mark -
#pragma mark Clean up of Structure Nodes

/*
 * Removes a node from the file system structure and releases its memory. This
 * happens only when the last instance of the file system is unmounted.
 */
void
release_node(sfssnode_t *root)
{
    sfssnode_t *stack[SYSFS_RELEASE_STACK_MAX];
    int sp = 0;

    if (root == NULL) {
        return;
    }

    /*
     * Detach the subtree's own root from its parent once, here. Every other node
     * is detached by its parent's iteration below, and must NOT be detached
     * again.
     *
     * That double-detach was a wild kernel write: a node is pushed only after
     * its parent has already TAILQ_REMOVE'd it, and the parent is freed at the
     * end of its own iteration - so removing the node "from its parent" when it
     * was later popped both removed it from a list it was no longer on AND
     * dereferenced an already-freed ssn_parent. TAILQ_REMOVE finishes with
     * *(elm)->prev = elm->next, i.e. a store through a stale pointer into
     * whatever had reused that memory. It corrupted the kernel heap on every
     * unmount: the machine froze instantly and completely, with no panic,
     * because it is silent corruption rather than a fault.
     */
    if (root->ssn_parent != NULL) {
        TAILQ_REMOVE(&root->ssn_parent->ssn_children, root, ssn_next);
        root->ssn_parent = NULL;
    }

    stack[sp++] = root;

    while (sp > 0) {
        sfssnode_t *node = stack[--sp];

        /*
         * Detach each child and queue it. Clearing ssn_parent as we go makes it
         * impossible for a popped node to reach back into its (soon to be freed)
         * parent.
         */
        sfssnode_t *child;
        while ((child = TAILQ_FIRST(&node->ssn_children)) != NULL) {
            TAILQ_REMOVE(&node->ssn_children, child, ssn_next);
            child->ssn_parent = NULL;

            if (sp >= SYSFS_RELEASE_STACK_MAX) {
                /*
                 * Cannot queue any more. Leaking this subtree is bad, but it is
                 * strictly better than overrunning the stack array - which would
                 * be the same class of memory corruption this function is fixing.
                 * The tree is a fixed, shallow skeleton, so this is unreachable
                 * in practice; raise SYSFS_RELEASE_STACK_MAX if that changes.
                 */
                LOG_ERR("release_node: node stack full (%d); leaking a subtree",
                        SYSFS_RELEASE_STACK_MAX);
                break;
            }
            stack[sp++] = child;
        }

        /*
         * Free this node's memory. It is already off its parent's list.
         */
        OSFree(node, sizeof(sfssnode_t), sysfs_osmalloc_tag);
    }
}