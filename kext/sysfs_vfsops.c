/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_vfsops.c
 *
 * VFS operations for the SysFS file system.
 */
#include <kern/locks.h>

#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSMalloc.h>

#include <libkext.h>

#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <fs/sysfs/sysfs.h>

#pragma mark Local Definitions

/*
 * The fixed mounted device name for this file system. The first
 * instance is called "sys", the second is "sys2" and so on.
 */
#define MOUNTED_DEVICE_NAME "sys"

/*
 * Block size for this file system. A meaningless value.
 */
#define BLOCK_SIZE 4096

/*
 * The number of hash buckets required. This *MUST* be
 * a power of two.
 */
#define HASH_BUCKET_COUNT (1 << 6)

/*
 * Each separate mount of the file system requires a unique id,
 * which is also used by every node in the file system. This is
 * equivalent to the dev_t associated with a real file system.
 */
STATIC int32_t sysfs_mount_id = 0;

#pragma mark -
#pragma mark External References

/*
 * Vnode ops descriptor for this file system.
 */
extern struct vnodeopv_desc *sysfs_vnodeops_list[1];

/*
 * Pointer to the constructed vnode operations vector. Set
 * when the file system is registered and used when creating
 * vnodes.
 */
extern int (**sysfs_vnodeop_p)(void *);

/*
 * Initialization routine. Only called once during the kext
 * start routine in sysfs.c - Included here as an external
 * reference for the sysfs_vfsops structure.
 */
extern int sysfs_init(struct vfsconf *vfsconf);

#pragma mark -
#pragma mark Function Prototypes

STATIC int sysfs_mount(struct mount *mp, vnode_t devvp, user_addr_t data, vfs_context_t context);
STATIC int sysfs_unmount(struct mount *mp, int mntflags, vfs_context_t context);
STATIC int sysfs_root(struct mount *mp, struct vnode **vpp, vfs_context_t context);
STATIC int sysfs_getattr(struct mount *mp, struct vfs_attr *fsap, vfs_context_t context);

STATIC void populate_statfs_info(struct mount *mp, struct vfsstatfs *statfsp);
STATIC void populate_vfs_attr(struct mount *mp, struct vfs_attr *fsap);
STATIC int sysfs_create_root_vnode(mount_t mp, sfsnode_t *snp, vnode_t *vpp);

#pragma mark -
#pragma mark VFS Operations and Entry Structures

vfstable_t sysfs_vfs_table_ref;

/*
 * VFS OPS structure maps VFS-level operations to
 * the functions that implement them, all of which
 * are in this file.
 */
struct vfsops sysfs_vfsops = {
    .vfs_mount          = sysfs_mount,
    .vfs_unmount        = sysfs_unmount,
    .vfs_root           = sysfs_root,
    .vfs_getattr        = sysfs_getattr,
    .vfs_init           = sysfs_init,
};

struct vfs_fsentry sysfs_vfsentry = {
    .vfe_vfsops         = &sysfs_vfsops,
    .vfe_vopcnt         = ARRAY_SIZE(sysfs_vnodeops_list),
    .vfe_opvdescs       = sysfs_vnodeops_list,
    .vfe_fstypenum      = 0,
    .vfe_fsname         = "sysfs",
    .vfe_flags          = SYSFS_VFS_FLAGS
};

#pragma mark -
#pragma mark Static Data

/*
 * Number of mounted instances of sysfs
 */
STATIC int mounted_instance_count = 0;

#pragma mark -
#pragma mark VFS Operations

/*
 * Performs the mount operation for the sysfs file system. Gets the options passed to the
 * mount(2) system call from user space, allocates a sfsmount_t structure, initializes
 * it and links it to the system's mount structure. On the first mount, the file system
 * node structure is created and file system initialization is completed.
 *
 * NOTE: mounts and unmounts are serialized by the mnt_rwlock in the VFS mount structure, so we do
 * not need to make this code reentrant or worry about being mounted and unmounted at the same time.
 */
STATIC int
sysfs_mount(struct mount *mp, __unused vnode_t devvp, user_addr_t data, __unused vfs_context_t context)
{
    sfsmount_t *sysfs_mp = MPTOPMP(mp);
    if (sysfs_mp == NULL) {
        /*
         * First mount. Get the mount options from user space.
         */
        sfsmount_args_t mount_args;
        mount_args.mnt_options = 0;
        if (data != USER_ADDR_NULL) {
            int error = copyin(data, &mount_args, sizeof(mount_args));
            if (error != 0) {
                printf("sysfs: failed to copyin mount options, using defaults\n");
                mount_args.mnt_options = 0;
            }
        }

        /*
         * Allocate the sysfs mount structure and link it to the VFS structure.
         */
        sysfs_mp = OSMalloc(sizeof(sfsmount_t), sysfs_osmalloc_tag);
        if (sysfs_mp == NULL) {
            printf("sysfs: Failed to allocate sfsmount_t");
            return ENOMEM;
        }

        OSAddAtomic(1, &sysfs_mount_id);
        sysfs_mp->pmnt_id = sysfs_mount_id;
        sysfs_mp->pmnt_flags = mount_args.mnt_options;
        sysfs_mp->pmnt_mp = mp;
        nanotime(&sysfs_mp->pmnt_mount_time);
        vfs_setfsprivate(mp, sysfs_mp);

        /*
         * Install sysfs-specific flags and augment the generic mount flags.
         * sysfs is a read-only synthetic view (as Linux /sys's root is) - it has
         * no writable nodes yet, so MNT_RDONLY is appropriate. It is revisited
         * when writable attributes arrive (the VFS layer rejects writes on a
         * read-only mount before they can reach vnop_write).
         */
        vfs_setflags(mp, MNT_RDONLY|MNT_NOSUID|MNT_NOEXEC|MNT_NODEV|MNT_NOATIME|MNT_LOCAL);

        /*
         * Increment the mounted instance count so that each mount of the file system
         * has a unique name as seen by the mount(1) command.
         */
        OSAddAtomic(1, &mounted_instance_count);

        /*
         * Set up the statfs structure in the VFS mount with mostly
         * boilerplate default values.
         */
        struct vfsstatfs *statfsp = vfs_statfs(mp);
        populate_statfs_info(mp, statfsp);

        /*
         * Complete setup of sysfs data. Does nothing after first mount.
         */
        sysfs_structure_init();

        /*
         * Initialize static data that is only required after an instance of the file
         * system has been mounted.
         */
        lck_mtx_lock(sfsnode_hash_mutex);
        if (sfsnode_hash_buckets == NULL) {
            /*
             * Set up the hash buckets only on first mount. Rather than define a
             * new BSD zone, we use the existing zone M_CACHE.
             */
            sfsnode_hash_buckets = hashinit(HASH_BUCKET_COUNT, M_CACHE, &sfsnode_hash_to_bucket_mask);
        }
        lck_mtx_unlock(sfsnode_hash_mutex);
    }

    return 0;
}

/*
 * Performs file system unmount. Clears out any cached vnodes, forcing reclaim, disconnects the
 * file system's sfsmount_t structure from the system mount structure and releases it.
 *
 * NOTE: mounts and unmounts are serialized by the mnt_rwlock in the VFS mount structure, so we do
 * not need to make this code reentrant or worry about being mounted and unmounted at the same time.
 */
STATIC int
sysfs_unmount(struct mount *mp, __unused int mntflags, __unused vfs_context_t context)
{
    sfsmount_t *sysfs_mp = MPTOPMP(mp);
    if (sysfs_mp != NULL) {
        /*
         * We are currently mounted. Release resources and disconnect.
         */

        /*
         * Flush out cached vnodes.
         */
        vflush(mp, NULLVP, FORCECLOSE);

        vfs_setfsprivate(mp, NULL);
        OSFree(sysfs_mp, sizeof(sfsmount_t), sysfs_osmalloc_tag);
        sysfs_mp = NULL;

        /*
         * Decrement mounted instance count.
         */
        OSAddAtomic(-1, &mounted_instance_count);
    }
    sysfs_structure_free();

    return 0;
}

/*
 * Gets the root vnode for the file system. If the vnode has already been
 * created, it may be still be in the cache. If not, or if this is the
 * first call to this function after mount, the root vnode and its
 * accompanying sfsnode_t are created and added to the cache.
 */
STATIC int
sysfs_root(struct mount *mp, vnode_t *vpp, __unused vfs_context_t context)
{
    vnode_t root_vnode;
    sfsnode_t *root_sfsnode;

    /*
     * Find the root vnode in the cache, or create it if it does not exist.
     */
    int error = sysfsnode_find(MPTOPMP(mp), SYSFS_ROOT_NODE_ID, sysfs_structure_root_node(),
                               &root_sfsnode, &root_vnode,
                               (create_vnode_func)&sysfs_create_root_vnode, mp);

    /*
     * Return the root vnode pointer to the caller, if it was created.
     */
    *vpp = error == 0 ? root_vnode : NULLVP;

    return error;
}

/*
 * Implementation of the VFS_GETATTR() function for the sysfs file system.
 * The vfs_attr structure is populated with values that have meaning for
 * sysfs. Most of them are dummy values and none of them change once the
 * file system has been mounted.
 */
STATIC int
sysfs_getattr(struct mount *mp, struct vfs_attr *fsap, __unused vfs_context_t context)
{
    populate_vfs_attr(mp, fsap);
    return 0;
}

#pragma mark -
#pragma mark Root Vnode Creation

/*
 * Creates the root vnode for an instance of the file system and
 * links it to its sfsnode_t. No internal locks are held when this
 * function is called.
 */
STATIC int
sysfs_create_root_vnode(mount_t mp, sfsnode_t *snp, vnode_t *vpp)
{
    struct vnode_fsparam vnode_create_params;

    memset(&vnode_create_params, 0, sizeof(vnode_create_params));
    vnode_create_params.vnfs_mp = mp;
    vnode_create_params.vnfs_vtype = VDIR;
    vnode_create_params.vnfs_str = "sysfs root vnode";
    vnode_create_params.vnfs_dvp = NULLVP;
    vnode_create_params.vnfs_fsnode = snp;
    vnode_create_params.vnfs_vops = sysfs_vnodeop_p;
    vnode_create_params.vnfs_markroot = 1;
    vnode_create_params.vnfs_flags = VNFS_CANTCACHE;

    /*
     * Create the vnode, if possible.
     */
    vnode_t root_vnode;
    int error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vnode_create_params, &root_vnode);

    /*
     * Return the root vnode pointer to the caller, if it was created.
     */
    *vpp = error == 0 ? root_vnode : NULLVP;

    return error;
}

#pragma mark -
#pragma mark File System Attributes

/*
 * Initializes a vfsstatfs structure with values that are
 * appropriate for a given mount of this file system. Most
 * values are fixed because this structure has limited meaning
 * for this file system.
 */
STATIC void
populate_statfs_info(struct mount *mp, struct vfsstatfs *statfsp)
{
    statfsp->f_bsize = BLOCK_SIZE;
    statfsp->f_iosize = BLOCK_SIZE;
    statfsp->f_blocks = 0;
    statfsp->f_bfree = 0;
    statfsp->f_bavail = 0;
    statfsp->f_bused = 0;
    statfsp->f_files = 0;
    statfsp->f_ffree = 0;

    /*
     * Compose fsid_t from the mount point id and the file system
     * type number, which was assigned when the file system was
     * registered. This pair of values just has to be unique.
     */
    statfsp->f_fsid.val[0] = MPTOPMP(mp)->pmnt_id;
    statfsp->f_fsid.val[1] = vfs_typenum(mp);

    bzero(statfsp->f_mntfromname, sizeof(statfsp->f_mntfromname));
    if (mounted_instance_count == 1) {
        /*
         * First mount -- just use the base name.
         */
        bcopy(MOUNTED_DEVICE_NAME, statfsp->f_mntfromname, strlen(MOUNTED_DEVICE_NAME));
    } else {
        /*
         * Subsequent mounts have the instance count + 1 added to the name.
         */
        snprintf(statfsp->f_mntfromname, sizeof(statfsp->f_mntfromname) - 1,
                 "%s%d", MOUNTED_DEVICE_NAME, mounted_instance_count);
    }
}

/*
 * Populates a vfs_attr structure with values that are appropriate
 * for this file system. As with the vfsstatfs structure, most of the
 * fields of the vfs_attr do not have any meaning for sysfs.
 */
STATIC void
populate_vfs_attr(struct mount *mp, struct vfs_attr *fsap)
{
    struct vfsstatfs *statfsp = vfs_statfs(mp);
    sfsmount_t *sysfs_mp = MPTOPMP(mp);

    VFSATTR_RETURN(fsap, f_objcount, 0);
    VFSATTR_RETURN(fsap, f_filecount, 0);
    VFSATTR_RETURN(fsap, f_dircount, 0);
    VFSATTR_RETURN(fsap, f_maxobjcount, 0);
    VFSATTR_RETURN(fsap, f_bsize, BLOCK_SIZE);
    VFSATTR_RETURN(fsap, f_iosize, BLOCK_SIZE);
    VFSATTR_RETURN(fsap, f_blocks, 0);
    VFSATTR_RETURN(fsap, f_bfree, 0);
    VFSATTR_RETURN(fsap, f_bavail, 0);
    VFSATTR_RETURN(fsap, f_bused, 0);
    VFSATTR_RETURN(fsap, f_files, 0);
    VFSATTR_RETURN(fsap, f_ffree, 0);
    VFSATTR_RETURN(fsap, f_fsid, statfsp->f_fsid);
    VFSATTR_RETURN(fsap, f_owner, statfsp->f_owner);
    VFSATTR_RETURN(fsap, f_create_time, sysfs_mp->pmnt_mount_time);
    VFSATTR_RETURN(fsap, f_modify_time, sysfs_mp->pmnt_mount_time);
    VFSATTR_RETURN(fsap, f_access_time, sysfs_mp->pmnt_mount_time);
}
