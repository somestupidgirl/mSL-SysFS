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
#include <libkern/OSMalloc.h>
#include <mach/boolean.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>

#pragma mark -
#pragma mark Function Prototypes

STATIC sfssnode_t *add_node(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, size_t size,
                        sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn);

STATIC sfssnode_t *add_directory(sfssnode_t *parent, const char *name, sfstype type, sfsbaseid_t node_id, uint16_t flags, boolean_t raw,
                        sysfs_node_size_fn node_size_fn, sysfs_read_data_fn node_read_data_fn);

STATIC void release_node(sfssnode_t *node);

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
        (void)add_directory(root_node, "class",      SFSdir, next_node_id++, 0, 0, NULL, NULL);

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
        (void)add_node(root_node, "devices", SFSdevice, next_node_id++, SSN_FLAG_DYNAMIC, 0, NULL, NULL);
        (void)add_directory(root_node, "firmware",   SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "fs",         SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "hypervisor", SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "kernel",     SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "module",     SFSdir, next_node_id++, 0, 0, NULL, NULL);
        (void)add_directory(root_node, "power",      SFSdir, next_node_id++, 0, 0, NULL, NULL);
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

#pragma mark -
#pragma mark Clean up of Structure Nodes

/*
 * Removes a node from the file system structure and releases its memory. This
 * happens only when the last instance of the file system is unmounted.
 */
void
release_node(sfssnode_t *snode)
{
    /*
     * Remove from its parent's children list, if it has one.
     */
    if (snode->ssn_parent != NULL) {
        TAILQ_REMOVE(&snode->ssn_parent->ssn_children, snode, ssn_next);
    }

    /*
     * Release all child nodes.
     */
    sfssnode_t *child = TAILQ_FIRST(&snode->ssn_children);
    while (child != NULL) {
        TAILQ_REMOVE(&snode->ssn_children, child, ssn_next);
        release_node(child);
        child = TAILQ_FIRST(&snode->ssn_children);
    }

    /*
     * Free this node's memory.
     */
    OSFree(snode, sizeof(sfssnode_t), sysfs_osmalloc_tag);
}
