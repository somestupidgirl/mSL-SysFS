/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_vnops.c
 *
 * Vnode operations for the SysFS file system.
 *
 * This is the scaffold reduction of the procfs vnode layer: the file system is
 * a static tree of directories, regular files and symlinks, with no dynamic
 * (per-process/thread/fd) expansion. The dynamic hooks the IORegistry-backed
 * passes need (a SSN_FLAG_DYNAMIC directory enumerating a live source at
 * lookup/readdir) are not wired in yet; when they arrive they slot into
 * sysfs_vnop_lookup / sysfs_vnop_readdir exactly where procfs handles its
 * process/thread markers.
 */
#include <libkern/libkern.h>

#include <sys/dirent.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/vnode.h>

#include <vfs/vfs_support.h>

#include <fs/sysfs/sysfs.h>

#pragma mark -
#pragma mark Local Definitions

/*
 * Read and execute permissions for all users (directories: 0555).
 */
#define READ_EXECUTE_ALL (S_IRUSR|S_IXUSR|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH)

/*
 * Read permission for all users (regular attribute files: 0444).
 */
#define READ_ALL (S_IRUSR|S_IRGRP|S_IROTH)

/*
 * All access for all users (symlinks: 0777 - the target decides real access).
 */
#define ALL_ACCESS_ALL (S_IRUSR|S_IWUSR|S_IXUSR|S_IRGRP|S_IWGRP|S_IXGRP|S_IROTH|S_IWOTH|S_IXOTH)

/*
 * Vnode Operations Function Descriptor
 */
#define VOPFUNC int (*)(void *)

/*
 * Structure used to hold the values needed to create a new vnode
 * corresponding to a sfsnode_t.
 */
typedef struct {
    vnode_t vca_parentvp; // Parent vnode.
} sysfs_vnode_create_args;

/*
 * Size of a buffer large enough to hold any structure-node name.
 */
static const int NAME_BUFFER_SIZE = MAX_STRUCT_NODE_NAME_LEN;

#pragma mark -
#pragma mark Function Prototypes

STATIC int sysfs_vnop_default(struct vnop_generic_args *arg);
STATIC int sysfs_vnop_lookup(struct vnop_lookup_args *ap);
STATIC int sysfs_vnop_getattr(struct vnop_getattr_args *ap);
STATIC int sysfs_vnop_reclaim(struct vnop_reclaim_args *ap);
STATIC int sysfs_vnop_readdir(struct vnop_readdir_args *ap);
STATIC int sysfs_vnop_getattrlistbulk(struct vnop_getattrlistbulk_args *ap);
STATIC int sysfs_vnop_readlink(struct vnop_readlink_args *ap);
STATIC int sysfs_vnop_read(struct vnop_read_args *ap);
STATIC int sysfs_vnop_open(struct vnop_open_args *ap);
STATIC int sysfs_vnop_close(struct vnop_close_args *ap);
STATIC int sysfs_vnop_access(struct vnop_access_args *ap);
STATIC int sysfs_vnop_inactive(struct vnop_inactive_args *ap);

STATIC inline int sysfs_calc_dirent_size(const char *name);
STATIC int sysfs_copyout_dirent(int type, uint64_t file_id, const char *name, uio_t uio, int *sizep, off_t seekoff);
STATIC int sysfs_create_vnode(sysfs_vnode_create_args *cap, sfsnode_t *snp, vnode_t *vpp);

#pragma mark -
#pragma mark Vnode Operations Structures

/*
 * Pointer to the constructed vnode operations vector. Set when the file system
 * is registered and used when creating vnodes.
 */
int (**sysfs_vnodeop_p)(void *);

/*
 * Entries for the vnode operations that this file system supports. This table is
 * converted to a fully-populated vnode operations vector when sysfs is
 * registered as a file system and a pointer to that vector is stored in
 * sysfs_vnodeop_p.
 *
 * The file system is read-only (the mount is MNT_RDONLY), so there is no write /
 * setattr entry; those arrive with the first writable attribute.
 */
struct vnodeopv_entry_desc sysfs_vnodeop_entries[] = {
    { .opve_op = &vnop_default_desc,            .opve_impl = (VOPFUNC) sysfs_vnop_default },            /* default */
    { .opve_op = &vnop_lookup_desc,             .opve_impl = (VOPFUNC) sysfs_vnop_lookup },             /* lookup */
    { .opve_op = &vnop_open_desc,               .opve_impl = (VOPFUNC) sysfs_vnop_open },               /* open */
    { .opve_op = &vnop_close_desc,              .opve_impl = (VOPFUNC) sysfs_vnop_close },              /* close */
    { .opve_op = &vnop_access_desc,             .opve_impl = (VOPFUNC) sysfs_vnop_access },             /* access */
    { .opve_op = &vnop_getattr_desc,            .opve_impl = (VOPFUNC) sysfs_vnop_getattr },            /* getattr */
    { .opve_op = &vnop_read_desc,               .opve_impl = (VOPFUNC) sysfs_vnop_read },               /* read */
    { .opve_op = &vnop_readdir_desc,            .opve_impl = (VOPFUNC) sysfs_vnop_readdir },            /* readdir */
    { .opve_op = &vnop_readdirattr_desc,        .opve_impl = (VOPFUNC) err_readdirattr },               /* readdirattr -> ENOTSUP, forces fallback to getdirentries64 */
    { .opve_op = &vnop_getattrlistbulk_desc,    .opve_impl = (VOPFUNC) sysfs_vnop_getattrlistbulk },    /* getattrlistbulk -> ENOTSUP, forces fallback to readdir+getattr */
    { .opve_op = &vnop_readlink_desc,           .opve_impl = (VOPFUNC) sysfs_vnop_readlink },           /* readlink */
    { .opve_op = &vnop_inactive_desc,           .opve_impl = (VOPFUNC) sysfs_vnop_inactive },           /* inactive */
    { .opve_op = &vnop_reclaim_desc,            .opve_impl = (VOPFUNC) sysfs_vnop_reclaim },            /* reclaim */
    { .opve_op = (struct vnodeop_desc*)NULL,    .opve_impl = (int (*)(void *))NULL }
};

/*
 * Descriptor used to create the vnode operations vector for sysfs from
 * sysfs_vnodeop_entries. Entries for operations that we do not support will get
 * appropriate defaults.
 */
struct vnodeopv_desc sysfs_vnodeop_opv_desc = {
    .opv_desc_vector_p  = &sysfs_vnodeop_p,
    .opv_desc_ops       = sysfs_vnodeop_entries
};

/*
 * List of descriptors used to build vnode operations vectors. Since we only have
 * one set of vnode operations, there is only one descriptor.
 */
struct vnodeopv_desc *sysfs_vnodeops_list[1] = {
    &sysfs_vnodeop_opv_desc,
};

#pragma mark -
#pragma mark Vnode Operations

/*
 * Vnode operations that don't require us to do anything.
 */
STATIC int
sysfs_vnop_open(__unused struct vnop_open_args *ap)
{
    return 0;
}

STATIC int
sysfs_vnop_access(__unused struct vnop_access_args *ap)
{
    return 0;
}

STATIC int
sysfs_vnop_close(__unused struct vnop_close_args *ap)
{
    return 0;
}

STATIC int
sysfs_vnop_inactive(__unused struct vnop_inactive_args *ap)
{
    /*
     * We do everything in sysfs_vnop_reclaim.
     */
    return 0;
}

STATIC int
sysfs_vnop_default(__unused struct vnop_generic_args *arg)
{
    return 0;
}

/*
 * We do not implement bulk attribute enumeration. Returning ENOTSUP here
 * overrides XNU's default getattrlistbulk implementation (which mis-packs our
 * entries and fails with ERANGE), causing callers such as ls(1)/fts(3) to fall
 * back to plain VNOP_READDIR + VNOP_GETATTR, which work correctly.
 */
STATIC int
sysfs_vnop_getattrlistbulk(__unused struct vnop_getattrlistbulk_args *ap)
{
    return ENOTSUP;
}

/*
 * Vnode lookup, called when resolving a path. Each invocation resolves one level
 * of path name and returns either an error or the vnode that corresponds to it,
 * with an additional iocount that must eventually be released by the caller.
 *
 * We are given the vnode of the path's directory and the path segment. The vnode
 * maps to a sfsnode_t, from which we get its sfssnode_t and hence the set of
 * valid child names. The scaffold tree is fully static, so a match is a simple
 * name comparison against the directory's structure children.
 */
STATIC int
sysfs_vnop_lookup(struct vnop_lookup_args *ap)
{
    char name[NAME_MAX + 1];
    int error = 0;
    struct componentname *cnp = ap->a_cnp;
    vnode_t dvp = ap->a_dvp; // Parent of the name to be looked up

    /*
     * The parent directory must not be NULL and the name length must be at
     * least 1.
     */
    if (dvp == NULLVP || vnode_vtype(dvp) != VDIR || cnp->cn_namelen < 1) {
        error = EINVAL;
        goto out;
    }

    /*
     * Get the sfsnode_t for the directory. This must not be NULL.
     */
    sfsnode_t *dir_snp = VTOSFS(dvp);
    if (dir_snp == NULL) {
        error = EINVAL;
        goto out;
    }

    /*
     * Preparation: get the component that we are looking up, clear the returned
     * vnode and ensure that nothing is added to the name cache.
     */
    strlcpy(name, cnp->cn_nameptr, min(sizeof(name), cnp->cn_namelen + 1));
    cnp->cn_flags &= ~MAKEENTRY;
    *ap->a_vpp = NULLVP;

    /*
     * SysFS file system mount.
     */
    sfsmount_t *mp = vfs_mp_to_sysfs_mp(vnode_mount(dvp));

    if (cnp->cn_flags & ISDOTDOT) {
        /*
         * We need the parent of the directory in "dvp". Get that by figuring out
         * what its node id would be.
         */
        sfsid_t parent_node_id;
        sysfs_get_parent_node_id(dir_snp, &parent_node_id);
        sfssnode_t *parent_snode = dir_snp->node_structure_node->ssn_parent;
        if (parent_snode == NULL) {
            parent_snode = dir_snp->node_structure_node;    /* root is its own parent */
        }

        sfsnode_t *target_sfsnode;
        vnode_t target_vnode;
        sysfs_vnode_create_args create_args;
        create_args.vca_parentvp = NULLVP;

        error = sysfsnode_find(mp, parent_node_id,
                               parent_snode,
                               &target_sfsnode,
                               &target_vnode,
                               (create_vnode_func)&sysfs_create_vnode,
                               &create_args);
        if (error == 0) {
            *ap->a_vpp = target_vnode;
        }
    } else if (cnp->cn_namelen == 1 && name[0] == '.') {
        /*
         * Looking for the current directory, so return "dvp" with an extra
         * iocount reference.
         */
        error = vnode_get(dvp);
        *ap->a_vpp = dvp;
    } else {
        /*
         * Match the name component against the child nodes of the directory's
         * structure node.
         */
        sfssnode_t *dir_snode = dir_snp->node_structure_node;
        sfssnode_t *match_node = NULL;
        sfsid_t match_node_id = { 0 };

        TAILQ_FOREACH(match_node, &dir_snode->ssn_children, ssn_next) {
            if (strcmp(name, match_node->ssn_name) == 0) {
                /*
                 * Name matched. Construct the node id from the matched node and
                 * the regid/objectid of the parent directory (both SYSFS_NO_* in
                 * the static skeleton).
                 */
                match_node_id.nodeid_base_id  = match_node->ssn_base_node_id;
                match_node_id.nodeid_regid    = dir_snp->node_id.nodeid_regid;
                match_node_id.nodeid_objectid = dir_snp->node_id.nodeid_objectid;
                break;
            }
        }

        /*
         * We have a match if match_node is not NULL.
         */
        if (match_node != NULL && error == 0) {
            /*
             * Look for the node in the cache, or create it if it is not there.
             * This also creates the vnode and increments its iocount.
             */
            sfsnode_t *target_sfsnode;
            vnode_t target_vnode;
            sysfs_vnode_create_args create_args;
            create_args.vca_parentvp = dvp;
            error = sysfsnode_find(mp, match_node_id,
                                   match_node,
                                   &target_sfsnode,
                                   &target_vnode,
                                   (create_vnode_func)&sysfs_create_vnode,
                                   &create_args);
            if (error == 0) {
                *ap->a_vpp = target_vnode;
            }
        } else if (error == 0) {
            // No match
            error = ENOENT;
        }
    }

out:
    return error;
}

/*
 * Implementation of the VNOP_READDIR operation. Given a directory vnode, returns
 * as many directory entries as will fit in the area described by a uio
 * structure. For the static scaffold tree the directory entries are simply the
 * children of the structure node (which include "." and "..").
 *
 * Each directory entry is made as small as possible by only including the
 * non-null part of the file name, so entries are of variable size. To read a
 * whole directory the caller may invoke this operation multiple times with a
 * different uio_offset; we always start from the first entry but only copy out
 * entries once the uio_offset value has been reached.
 */
STATIC int
sysfs_vnop_readdir(struct vnop_readdir_args *ap)
{
    vnode_t vp = ap->a_vp;
    if (vnode_vtype(vp) != VDIR) {
        return ENOTDIR;
    }

    sfsnode_t *dir_snp = VTOSFS(vp);
    sfssnode_t *dir_snode = dir_snp->node_structure_node;

    int numentries = 0;
    int error = 0;
    uio_t uio = ap->a_uio;
    off_t nextpos = 0;
    off_t startpos = uio_offset(uio);

    sfssnode_t *snode = TAILQ_FIRST(&dir_snode->ssn_children);
    while (snode != NULL && uio_resid(uio) > 0) {
        uint64_t regid = dir_snp->node_id.nodeid_regid;
        uint64_t objectid = dir_snp->node_id.nodeid_objectid;
        sfsbaseid_t base_node_id = snode->ssn_base_node_id;
        const char *name = snode->ssn_name;
        int type = DT_REG;

        switch (snode->ssn_node_type) {
        case SFSroot: /* Indicates structure error - skip it. */
            printf("sysfs_vnop_readdir: ERROR: found SFSroot\n");
            snode = TAILQ_NEXT(snode, ssn_next);
            continue;

        case SFSdir:            /* FALLTHROUGH */
        case SFSdevice:         /* FALLTHROUGH */
        case SFSsubsystem:
            type = DT_DIR;
            break;

        case SFSfile:           /* FALLTHROUGH */
        case SFSattr:           /* FALLTHROUGH */
        case SFSmodule:
            type = DT_REG;
            break;

        case SFSlink:
            type = DT_LNK;
            break;

        case SFSdirthis:
            type = DT_DIR;
            /*
             * We need to use the node id of the directory node for this case.
             */
            regid = dir_snp->node_id.nodeid_regid;
            objectid = dir_snp->node_id.nodeid_objectid;
            base_node_id = dir_snp->node_id.nodeid_base_id;
            break;

        case SFSdirparent:
            type = DT_DIR;
            /*
             * We need to use the node id of the directory's parent for this case.
             */
            {
                sfsid_t parent_node_id;
                sysfs_get_parent_node_id(dir_snp, &parent_node_id);
                regid = parent_node_id.nodeid_regid;
                objectid = parent_node_id.nodeid_objectid;
                base_node_id = parent_node_id.nodeid_base_id;
            }
            break;
        }

        /*
         * Always compute size so nextpos advances correctly even when this entry
         * falls before startpos (i.e. is being skipped on a resumed readdir).
         */
        int size = sysfs_calc_dirent_size(name);
        if (nextpos >= startpos) {
            error = sysfs_copyout_dirent(type, sysfs_get_fileid(regid, objectid, base_node_id),
                                         name, uio, &size, nextpos + size);
            if (size == 0 || error != 0) {
                break;
            }
            numentries++;
        }
        nextpos += size;

        /*
         * Continue with the next node.
         */
        snode = TAILQ_NEXT(snode, ssn_next);
    }

    /*
     * Set output values for the next pass.
     */
    uio_setoffset(uio, nextpos);
    *ap->a_eofflag = snode == NULL; // EOF if we handled the last entry
    *ap->a_numdirent = numentries;

    return error;
}

/*
 * Calculates the packed size for a directory entry for a given file name. The
 * size is the sum of the fixed part of the dirent structure plus the space
 * required for the null-terminated name, rounded up to a multiple of 8 bytes
 * (required by XNU's direntry validation).
 */
STATIC int
sysfs_calc_dirent_size(const char *name)
{
    struct direntry entry;
    return (int)(sizeof(struct direntry) - sizeof(entry.d_name) + ((strlen(name) + 1 + 7) & ~(size_t)7));
}

/*
 * Copies a directory entry out to the area described by a uio structure and
 * updates that structure. No copy is performed if there is not enough space to
 * copy the entire structure.
 */
STATIC int
sysfs_copyout_dirent(int type, uint64_t file_id, const char *name, uio_t uio, int *sizep, off_t seekoff)
{
    struct direntry entry;
    bzero(&entry, sizeof(entry));

    entry.d_type   = (uint8_t)type;
    entry.d_ino    = file_id;
    entry.d_seekoff = (uint64_t)seekoff;
    entry.d_namlen = (uint16_t)strlen(name);
    strlcpy(entry.d_name, name, entry.d_namlen + 1);

    int size = *sizep;
    entry.d_reclen = (uint16_t)size;

    int error = 0;
    if (size <= uio_resid(uio)) {
        error = uiomove((const char *)&entry, size, uio);
        *sizep = size;
    } else {
        *sizep = 0;
    }
    return error;
}

/*
 * Gets the attributes for a node, as seen by the stat(2) system call. Many
 * attributes don't make sense for sysfs nodes, so are not set. In other cases,
 * the values are fixed. sysfs is a world-readable, root-owned synthetic view
 * (as Linux /sys is): directories are 0555, attribute files 0444, symlinks 0777.
 */
STATIC int
sysfs_vnop_getattr(struct vnop_getattr_args *ap)
{
    vnode_t vp = ap->a_vp;
    sfsnode_t *sysfs_node = VTOSFS(vp);
    sfssnode_t *snode = sysfs_node->node_structure_node;
    sfstype node_type = snode->ssn_node_type;
    sfsmount_t *pmp = vfs_mp_to_sysfs_mp(vnode_mount(vp));

    struct vnode_attr *vap = ap->a_vap;

    /*
     * Mode from the node type.
     */
    mode_t mode;
    if (node_type == SFSlink) {
        mode = ALL_ACCESS_ALL;                  /* target decides real access */
    } else if (sysfs_is_directory_type(node_type)) {
        mode = READ_EXECUTE_ALL;                /* 0555 */
    } else {
        mode = READ_ALL;                        /* 0444 */
    }
    VATTR_RETURN(vap, va_mode, mode);

    /*
     * Generic attributes.
     */
    VATTR_RETURN(vap, va_type, sysfs_allocvp(node_type));                    /* File type */
    VATTR_RETURN(vap, va_fsid, pmp->pmnt_id);                               /* File system id */
    VATTR_RETURN(vap, va_fileid, sysfs_get_node_fileid(sysfs_node));        /* Unique file id */
    VATTR_RETURN(vap, va_data_size,
        sysfs_get_node_size_attr(sysfs_node, vfs_context_ucred(ap->a_context)));  /* File size */

    /*
     * These files are generated on read, so a live timestamp is more meaningful
     * than a frozen one (as Linux's /sys reports).
     */
    struct timespec now;
    nanotime(&now);
    VATTR_RETURN(vap, va_access_time, now);
    VATTR_RETURN(vap, va_change_time, now);
    VATTR_RETURN(vap, va_create_time, now);
    VATTR_RETURN(vap, va_modify_time, now);

    /*
     * sysfs is owned by root (uid 0 / gid 0), as on Linux.
     */
    VATTR_RETURN(vap, va_uid, (uid_t)0);
    VATTR_RETURN(vap, va_gid, (gid_t)0);

    return 0;
}

/*
 * Reads the content of a symbolic link. No symlinks exist in the scaffold tree
 * (the class/bus/block/dev views arrive with the IORegistry passes), so this
 * currently rejects every node; it is retained so the readlink op is present and
 * ready for those targets.
 */
STATIC int
sysfs_vnop_readlink(struct vnop_readlink_args *ap)
{
    vnode_t vp = ap->a_vp;
    sfsnode_t *snp = VTOSFS(vp);
    sfssnode_t *snode = snp->node_structure_node;

    if (snode->ssn_node_type == SFSlink) {
        /*
         * No symlink targets are defined yet.
         */
        return ENOENT;
    }
    return EINVAL;
}

/*
 * Reads a node's data. The read operation is delegated to a function that's held
 * in the node's sfssnode_t. For nodes that can't be read, the function is NULL
 * and EINVAL is returned, except for a directory, for which the error is EISDIR.
 * The scaffold skeleton has no read functions, so every regular node reads as
 * EINVAL and every directory as EISDIR until content is added.
 */
STATIC int
sysfs_vnop_read(struct vnop_read_args *ap)
{
    vnode_t vp = ap->a_vp;
    sfsnode_t *snp = VTOSFS(vp);
    sfssnode_t *snode = snp->node_structure_node;
    sysfs_read_data_fn read_data_fn = snode->ssn_read_data_fn;

    int error = EINVAL;
    if (sysfs_is_directory_type(snode->ssn_node_type)) {
        error = EISDIR;
    } else if (read_data_fn != NULL) {
        error = read_data_fn(snp, ap->a_uio, ap->a_context);
    }
    return error;
}

/*
 * Reclaims a vnode and its associated sfsnode_t when it's no longer needed by
 * the kernel file system code. Removes the sfsnode_t from the hash table,
 * removes the file system reference and breaks the link between the vnode and
 * the sfsnode_t.
 */
STATIC int
sysfs_vnop_reclaim(struct vnop_reclaim_args *ap)
{
    sfsnode_t *snp = VTOSFS(ap->a_vp);

    if (snp != NULL) {
        /*
         * Lock to manipulate the hash table.
         */
        lck_mtx_lock(sfsnode_hash_mutex);

        /*
         * Remove the node from the hash table and free it.
         */
        sysfsnode_free_node(snp);

        /*
         * CAUTION: snp is now invalid.
         */
        snp = NULL;
        lck_mtx_unlock(sfsnode_hash_mutex);
    }

    /*
     * Remove the file system reference that we added when we created the vnode.
     */
    vnode_removefsref(ap->a_vp);

    /*
     * Clear the link to the sfsnode_t since the vnode will no longer be linked
     * to it.
     */
    vnode_clearfsnode(ap->a_vp);

    return 0;
}

#pragma mark -
#pragma mark Helper Functions

/*
 * Creates a vnode with given properties, which depend on the vnode type.
 */
STATIC int
sysfs_create_vnode(sysfs_vnode_create_args *cap, sfsnode_t *snp, vnode_t *vpp)
{
    sfssnode_t *snode = snp->node_structure_node;
    struct vnode_fsparam vnode_create_params;

    memset(&vnode_create_params, 0, sizeof(vnode_create_params));
    vnode_create_params.vnfs_mp = vnode_mount(cap->vca_parentvp);
    vnode_create_params.vnfs_vtype = sysfs_allocvp(snode->ssn_node_type);
    vnode_create_params.vnfs_str = "sysfs vnode";
    vnode_create_params.vnfs_dvp = cap->vca_parentvp;
    vnode_create_params.vnfs_fsnode = snp;
    vnode_create_params.vnfs_vops = sysfs_vnodeop_p;
    vnode_create_params.vnfs_markroot = 0;
    vnode_create_params.vnfs_flags = VNFS_CANTCACHE;

    /*
     * Create the vnode, if possible.
     */
    vnode_t new_vnode;
    int error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vnode_create_params, &new_vnode);

    /*
     * Return the vnode pointer to the caller, if it was created.
     */
    *vpp = error == 0 ? new_vnode : NULLVP;

    return error;
}
