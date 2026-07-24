/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_subr.c
 *
 * Utility functions for the SysFS file system.
 */
#include <kern/assert.h>

#include <libkern/OSMalloc.h>

#include <sys/malloc.h>
#include <sys/uio.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>

/*
 * Gets the vnode type that is appropriate for a given structure node type.
 */
enum vtype
sysfs_allocvp(sfstype sfs_type)
{
    switch (sfs_type) {
    case SFSroot:           /* FALLTHROUGH */
    case SFSdir:            /* FALLTHROUGH */
    case SFSdirthis:        /* FALLTHROUGH */
    case SFSdirparent:      /* FALLTHROUGH */
    case SFSdevice:         /* FALLTHROUGH */
    case SFSsubsystem:
        return VDIR;

    case SFSfile:           /* FALLTHROUGH */
    case SFSattr:           /* FALLTHROUGH */
    case SFSmodule:
        return VREG;

    case SFSlink:
        return VLNK;
    }

    /*
     * Unknown type: make it a file.
     */
    return VREG;
}

/*
 * Gets the file id for a given node. There is no obvious way to create a unique
 * and reproducible file id for a node that doesn't have any persistent storage,
 * so we synthesize one based on the base node id from the file system structure,
 * the backing IORegistry entry id (if any) and the owning object id. This may
 * not be unique because we can only include part of each id. It should, however,
 * be good enough.
 */
uint64_t
sysfs_get_node_fileid(sfsnode_t *snp)
{
    sfsid_t node_id = snp->node_id;
    return sysfs_get_fileid(node_id.nodeid_regid, node_id.nodeid_objectid,
                            snp->node_structure_node->ssn_base_node_id);
}

/*
 * Constructs a file id for a given registry entry id, object id and structure
 * node base id. This may not be unique because we can only include part of the
 * ids. It should, however, be good enough.
 */
uint64_t
sysfs_get_fileid(uint64_t regid, uint64_t objectid, sfsbaseid_t base_id)
{
    uint64_t id = base_id;
    id |= regid << 8;
    id |= objectid << 24;
    return id;
}

/*
 * Attempts to convert a string to a positive integer. Returns the value, or -1
 * if the string does not start with an integer value. *end_ptr is set to point
 * to the first non-numeric character encountered.
 */
int
sysfs_atoi(const char *p, const char **end_ptr)
{
    int value = 0;
    const char *next = p;
    char c;

    while ((c = *next++) != (char)0 && c >= '0' && c <= '9') {
        value = value * 10 + c - '0';
    }
    *end_ptr = next - 1;

    /*
     * Invalid if the first character was not a digit.
     */
    return next == p + 1 ? -1 : value;
}

/*
 * Gets the value of the st_size field of a node's attributes. POSIX allows us to
 * use this value as we choose. This function calculates the appropriate size for
 * a node by calling that node's ssn_getsize_fn() function. For directories, the
 * size reported by all non-directory child nodes is aggregated to get the result.
 */
size_t
sysfs_get_node_size_attr(sfsnode_t *snp, kauth_cred_t creds)
{
    sfssnode_t *snode = snp->node_structure_node;
    sfstype node_type = snode->ssn_node_type;

    /*
     * In the special cases of "." and "..", we need to first move up
     * to the parent and grandparent structure node to get the correct result.
     */
    if (node_type == SFSdirthis) {
        snode = snode->ssn_parent;
    } else if (node_type == SFSdirparent) {
        snode = snode->ssn_parent;
        if (snode != NULL && snode->ssn_node_type != SFSroot) {
            snode = snode->ssn_parent;
        }
    }

    assert(snode != NULL);

    /*
     * For file types, get the size from the node itself. For directory types,
     * get the size by traversing child structure nodes.
     */
    size_t size = 0;
    if (sysfs_is_directory_type(node_type)) {
        /*
         * Directory
         */
        sfssnode_t *next_snode;
        TAILQ_FOREACH(next_snode, &snode->ssn_children, ssn_next) {
            sysfs_node_size_fn node_size_fn = next_snode->ssn_getsize_fn;
            size += node_size_fn == NULL ? 1 : node_size_fn(snp, creds);
        }
    } else {
        /*
         * File or symlink
         */
        sysfs_node_size_fn node_size_fn = snode->ssn_getsize_fn;
        size = node_size_fn == NULL ? snode->ssn_node_size : node_size_fn(snp, creds);
    }

    return size;
}

/*
 * Copies data from the local buffer "data" into the area described by a uio
 * structure. The first byte of "data" is assumed to correspond to a zero offset,
 * so if the uio structure has its uio_offset set to N, the first byte of data
 * that will be copied is at data[N].
 */
int
sysfs_copy_data(const char *data, int data_len, uio_t uio)
{
    int error = 0;

    if (data == NULL) {
        return EFAULT;
    }

    off_t start_offset = uio_offset(uio);
    if (start_offset >= data_len) {
        /* Nothing to copy */
        return error;
    }

    int len = data_len - (int)start_offset;
    if (len < 0) {
        return EINVAL;
    }

    if ((len >= 0) && (data != NULL)) {
        error = uiomove(data + start_offset, len, uio);
    } else if ((len >= 0) && (data == NULL)) {
        error = EFAULT;
    } else if ((len <= 0) && (data != NULL)) {
        error = EINVAL;
    } else {
        error = ENOTSUP;
    }

    return error;
}
