/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs.h
 *
 * Definitions for the sysfs file system. The content of this
 * header is available to both user-level and kernel components.
 *
 * sysfs is the Linux-style /sys for macOS: a filesystem view of the system's
 * device model. Its heart is a mirror of the IOKit registry (IORegistry) under
 * /sys/devices - IOKit registry entries become directories, IOKit properties
 * become attribute files - with class/, bus/, block/ and dev/ presented as
 * symbolic-link views into it. This header defines the node model shared by the
 * kext and (later) the sysfsd userspace daemon; it deliberately mirrors the
 * sibling mSL/ProcFS layout so the two modules stay recognisably one project.
 *
 * This is the scaffold: the tree is the fixed Linux /sys skeleton of empty
 * top-level directories. The dynamic IORegistry-backed content is a later pass,
 * but the node identity below already reserves the IORegistryEntryID key it
 * needs, so it drops in without reworking this model.
 */
#ifndef sysfs_h
#define sysfs_h

#pragma mark -
#pragma mark Definitions for both user-level and kernel components

/*
 * Lock group name
 */
#define SYSFS_LCKGRP_NAME      BUNDLEID_S ".lckgrp"

/*
 * Mount option flags. None are defined yet - sysfs has no process-permission
 * model like procfs's "noprocperms" - but the plumbing is kept so options can
 * be added (e.g. hiding subsystems) without a wire-format change.
 */
#define SYSFS_MOPT_NONE         0
#define MOPT_SYSFS              { "sysfsopt", 1, SYSFS_MOPT_NONE, 0 }

#define SYSFS_MAXNAMLEN    255

/*
 * The sysfs mount structure, created by mount_sysfs
 * and passed to the kernel by the mount(2) system call.
 */
typedef struct sfsmount_args {
    int mnt_options;      /* The sysfs mount options. */
} sfsmount_args_t;

#pragma mark -
#pragma mark Internal Definitions - Kernel Only

#ifndef __FSBUNDLE__

#include <kern/locks.h>
#include <libkern/OSMalloc.h>
#include <libkext.h>
#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#pragma mark -
#pragma mark External References

/*
 * Lock used to protect the hash table.
 */
extern lck_grp_t *sfsnode_lck_grp;
extern lck_mtx_t *sfsnode_hash_mutex;

/*
 * Tag used for memory allocation.
 */
extern OSMallocTag sysfs_osmalloc_tag;

/*
 * The buckets for the sfsnode hash table. The number of buckets
 * is always a power of two.
 */
extern LIST_HEAD(sysfs_hash_head, sfsnode) *sfsnode_hash_buckets;

/*
 * The mask used to get the bucket number from a sfsnode hash.
 */
extern u_long sfsnode_hash_to_bucket_mask;

#pragma mark -
#pragma mark Type Definitions

/*
 * The different types of node in a sysfs filesystem.
 *
 * The first group is the fixed skeleton this scaffold pass builds. The second
 * group is reserved for the IORegistry-backed passes (declared now so the node
 * model, allocvp and directory-type predicate are stable); no structure node
 * uses those types yet.
 */
typedef enum {
    SFSroot,        /* the filesystem root (/sys) */
    SFSdir,         /* an ordinary directory */
    SFSfile,        /* a regular attribute file */
    SFSdirthis,     /* representation of "." */
    SFSdirparent,   /* representation of ".." */
    SFSlink,        /* a symbolic link (class/bus/block/dev views) */

    /* --- reserved for later passes (IORegistry mapping); unused in scaffold. */
    SFSdevice,      /* a /sys/devices entry (one IORegistry object) */
    SFSattr,        /* a device attribute file (one IOKit property) */
    SFSsubsystem,   /* a class/bus grouping directory */
    SFSmodule,      /* a /sys/module entry (one loaded kext) */
} sfstype;

typedef struct sfsnode sfsnode_t;
typedef struct sfsid sfsid_t;
typedef struct sfsmount sfsmount_t;
typedef struct sfssnode sfssnode_t;
typedef struct sysfs_hash_head sysfs_hash_head;

/*
 * Callback function used to create vnodes, called from within the
 * sysfsnode_find() function. "params" is used to pass the details that
 * the function needs in order to create the correct vnode. It is obtained
 * from the "create_vnode_params" argument passed to sysfsnode_find(),
 * "snp" is a pointer to the sfsnode_t that the vnode should be linked to
 * and "vpp" is where the created vnode will be stored, if the call was
 * successful. Returns 0 on success or an error code (from errno.h) if not.
 */
typedef int (*create_vnode_func)(void *params, sfsnode_t *snp, vnode_t *vpp);

/*
 * Type for the base node id field of a structure node.
 */
typedef uint16_t sfsbaseid_t;

/*
 * Type of a function that reports the size for a sysfs node.
 */
typedef size_t (*sysfs_node_size_fn)(sfsnode_t *snp, kauth_cred_t creds);

/*
 * Type of a function that reads the data for a sysfs node.
 */
typedef int (*sysfs_read_data_fn)(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);

#pragma mark -
#pragma mark Common Definitions

/*
 * VFS flags
 */
#define SYSFS_VFS_FLAGS  ( \
        VFS_TBL64BITREADY       | \
        VFS_TBLFSNODELOCK       | \
        VFS_TBLLOCALVOL         | \
        VFS_TBLNOTYPENUM        | \
        VFS_TBLNOMACLABEL       | \
        VFS_TBLREADDIR_EXTENDED | \
        0                         \
)

/*
 * Bit values for the ssn_flags field. Reserved for the dynamic passes: a
 * structure node marked SSN_FLAG_DYNAMIC expands at readdir/lookup from a live
 * source (the IORegistry) rather than from static structure children. Nothing
 * sets it in the scaffold; it is propagated to descendants like procfs's
 * process/thread flags so a subtree can be recognised by its flags alone.
 */
#define SSN_FLAG_DYNAMIC    (1 << 0)

/*
 * Special values for the nodeid_regid and nodeid_objectid fields.
 */
#define SYSFS_NO_REGID      ((uint64_t)0)   /* node not backed by an IORegistry entry */
#define SYSFS_NO_OBJECTID   ((uint64_t)0)   /* node has no sub-object/attribute id */

/*
 * Root node id value.
 */
#define SYSFS_ROOT_NODE_BASE_ID ((sfsbaseid_t)1)

/*
 * Largest name of a structure node, including the terminating NUL. This must
 * exceed the longest name registered in sysfs_structure.c; add_node() logs any
 * name it has to truncate (a truncated name silently collides with any sibling
 * sharing the prefix). See the procfs sibling for the incident that motivated
 * the length check.
 */
#define MAX_STRUCT_NODE_NAME_LEN 32

#pragma mark -
#pragma mark Structure Definitions

/*
 * Definitions for the data structures that determine the layout of nodes in the
 * sysfs file system. The layout is constructed by building a tree of sfssnode_t
 * structures. The layout is the same for each file system instance and is
 * created when the first instance of the file system is mounted.
 *
 * An entry in the sysfs file system layout. All fields of this structure are
 * set on creation and do not change, so no locking is required to access them.
 *
 * The ssn_node_type field is the type of the structure node. These types are
 * mapped to the usual vnode types by the file system when getting node
 * attributes and are used during node lookup and other vnode operations.
 *
 * The ssn_base_node_id field is a unique value that becomes part of the full id
 * of any sfsnode_t that is created from this structure node.
 *
 * The SSN_FLAG_* flag values of a node are propagated to all descendent nodes,
 * so a subtree's nature can be determined just by examining ssn_flags.
 */
struct sfssnode {
    sfstype                           ssn_node_type;
    char                              ssn_name[MAX_STRUCT_NODE_NAME_LEN];
    sfsbaseid_t                       ssn_base_node_id;   /* Base node id - unique. */
    uint16_t                          ssn_flags;          /* Flags - SSN_FLAG_* */

    /*
     * Structure linkage. Immutable once set.
     */
    struct sfssnode                  *ssn_parent;   /* The parent node in the structure */
    TAILQ_ENTRY(sfssnode)             ssn_next;     /* Next sibling node within structure parent. */
    TAILQ_HEAD(sfschildren, sfssnode) ssn_children; /* Children of this structure node. */

    /*
     * Function hooks. Set to null to use the defaults.
     * The node's size value. This is the size value for the node itself.
     * For directory nodes, the sum of the size values of all of its children is
     * used as the actual size, so this value has meaning only for nodes of type
     * SFSfile. It is not used if the ssn_getsize_fn field is set.
     */
    size_t                            ssn_node_size;

    /*
     * Gets the value for the node's size attribute. If NULL, ssn_node_size
     * is used instead.
     */
    sysfs_node_size_fn                ssn_getsize_fn;

    /*
     * Reads the file content. NULL for directories and for the empty scaffold
     * skeleton (a read then yields EISDIR / EINVAL respectively).
     */
    sysfs_read_data_fn                ssn_read_data_fn;
};

/*
 * Composite identifier for a node in the sysfs file system. There must only
 * ever be one node for each unique identifier in any given instance of the file
 * system (i.e. per mount).
 *
 * Unlike procfs (which keys on pid), sysfs keys on the IOKit registry: a device
 * node carries the backing object's IORegistryEntryID in nodeid_regid, and an
 * attribute/child within that object is distinguished by nodeid_objectid.
 * Skeleton nodes with no backing object use SYSFS_NO_REGID / SYSFS_NO_OBJECTID.
 */
struct sfsid {
    uint64_t      nodeid_regid;       /* Backing IORegistryEntryID, or SYSFS_NO_REGID. */
    uint64_t      nodeid_objectid;    /* Sub-object/attribute within the entry, or SYSFS_NO_OBJECTID. */
    sfsbaseid_t   nodeid_base_id;     /* The id of the structure node to which this node is linked. */
};

/*
 * sysfs per-mount data structure. Typically there is only one instance of this
 * file system, but the implementation does not preclude multiple mounts.
 */
struct sfsmount {
    int32_t           pmnt_id;            /* A unique identifier for this mount. Shared by all nodes. */
    int               pmnt_flags;         /* Flags, set from the mount command (SYSFS_MOPT_*). */
    struct mount     *pmnt_mp;            /* VFS-level mount structure. */
    struct timespec   pmnt_mount_time;    /* Time at which the file system was mounted. */
};

/*
 * The filesystem-dependent vnode private data for sysfs. There is one instance
 * of this structure for each active node.
 */
struct sfsnode {
    /*
     * Linkage for the node hash. Protected by the node hash lock.
     */
    LIST_ENTRY(sfsnode) node_hash;

    /*
     * Pointer to the associated vnode. Protected by the node hash lock.
     */
    vnode_t             node_vnode;

    /* Records whether this node is currently being attached to a vnode.
     * Only one thread can be allowed to link the node to a vnode. If a
     * thread that wants to create a sfsnode and link it to a vnode
     * finds this field set to true, it must release the node hash lock
     * and wait until the field is reset to false, then check again whether
     * some or all of the work that it needed to do has been completed.
     * Protected by the node hash lock.
     */
    boolean_t           node_attaching_vnode;

    /*
     * Records whether a thread is awaiting the outcome of vnode attachment.
     * Protected by the node hash lock.
     */
    boolean_t           node_thread_waiting_attach;

    /*
     * node_mnt_id and node_id taken together uniquely identify a node. There
     * must only ever be one sfsnode instance (and hence one vnode) for each
     * (node_mnt_id, node_id) combination. The node_mnt_id value can be obtained
     * from the pmnt_id field of the sfsmount structure for the owning mount.
     */
    int32_t             node_mnt_id;            /* Identifier of the owning mount. */
    sfsid_t             node_id;                /* The identifer of this node. */

    /*
     * Pointer to the sfssnode_t for this node.
     */
    sfssnode_t         *node_structure_node;    /* Set when allocated, never changes. */
};

#pragma mark -
#pragma mark Macros

#define CNEQ(cnp, s, len) \
     ((cnp)->cn_namelen == (len) && \
      (memcmp((s), (cnp)->cn_nameptr, (len)) == 0))

/*
 * Convert between sfsmount and vfs mount.
 */
#define MPTOPMP(mp)    vfs_mp_to_sysfs_mp(mp)
#define PMPTOMP(smp)   sysfs_mp_to_vfs_mp(smp)

/*
 * Convert between sfsnode and vnode.
 */
#define VTOSFS(vp)     vnode_to_sysfsnode(vp)
#define SFSTOV(snp)    sfsnode_to_vnode(snp)

/*
 * Zero out vnode attributes.
 */
#define VATTR_NULL(vap) bzero(vap, sizeof(struct vnode_attr))

#pragma mark -
#pragma mark Inline Conversion Functions

/*
 * Convert from sysfs vnode pointer to VFS vnode pointer.
 */
static inline vnode_t
sfsnode_to_vnode(sfsnode_t *snp)
{
    return snp->node_vnode;
}

/*
 * Convert from VFS vnode pointer to sysfs vnode pointer.
 */
static inline sfsnode_t *
vnode_to_sysfsnode(vnode_t vp)
{
    return (sfsnode_t *)vnode_fsnode(vp);
}

/*
 * Convert from sysfs mount pointer to VFS mount pointer.
 */
static inline struct mount *
sysfs_mp_to_vfs_mp(sfsmount_t *smp)
{
    return smp->pmnt_mp;
}

/*
 * Convert from VFS mount pointer to sysfs mount pointer.
 */
static inline sfsmount_t *
vfs_mp_to_sysfs_mp(struct mount *vmp)
{
    return (sfsmount_t *)vfs_fsprivate(vmp);
}

#pragma mark -
#pragma mark Inline Convenience Functions

/*
 * Returns whether a given node type represents a directory.
 */
static inline boolean_t
sysfs_is_directory_type(sfstype type)
{
    return type == SFSroot || type == SFSdir
        || type == SFSdirthis || type == SFSdirparent
        || type == SFSdevice || type == SFSsubsystem;
}

#pragma mark -
#pragma mark Global Definitions

/*
 * Identifier for the root node of the file system.
 */
extern const sfsid_t SYSFS_ROOT_NODE_ID;

/*
 * Public API - node management (sysfs_node.c).
 */
extern int sysfsnode_find(sfsmount_t *smp, sfsid_t node_id, sfssnode_t *snode, sfsnode_t **snpp,
                          vnode_t *vnpp, create_vnode_func create_vnode_func, void *create_vnode_params);
extern void sysfsnode_free_node(sfsnode_t *sfsnode);
extern void sysfs_get_parent_node_id(sfsnode_t *snp, sfsid_t *idp);

/*
 * Structure tree (sysfs_structure.c).
 */
extern sfssnode_t *sysfs_structure_root_node(void);
extern void sysfs_structure_init(void);
extern void sysfs_structure_free(void);

/*
 * Subroutines (sysfs_subr.c).
 */
extern enum vtype sysfs_allocvp(sfstype type);
extern int        sysfs_copy_data(const char *data, int data_len, uio_t uio);
extern uint64_t   sysfs_get_node_fileid(sfsnode_t *snp);
extern uint64_t   sysfs_get_fileid(uint64_t regid, uint64_t objectid, sfsbaseid_t base_id);
extern int        sysfs_atoi(const char *p, const char **end_ptr);
extern size_t     sysfs_get_node_size_attr(sfsnode_t *snp, kauth_cred_t creds);

/*
 * Content for /sys/devices/system (sysfs_system.c).
 *
 * sysfs_do_zero_count renders an always-zero counter as Linux renders a sysfs
 * integer attribute ("0\n"); SYSFS_ZERO_COUNT_LEN is the length it produces, so
 * a node using it can report the right size without a size function.
 */
#define SYSFS_ZERO_COUNT_LEN 2
extern int sysfs_do_zero_count(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);

/*
 * The remaining /sys/devices/system content. These render at read time, so their
 * nodes carry no fixed ssn_node_size; stat() reports 0 for them, which is what
 * Linux's own sysfs attribute files report (they are generated on read, and
 * every tool reads them to EOF rather than trusting st_size).
 */
extern int sysfs_do_cpulist(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_cpumap(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_node_meminfo(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_node_distance(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_node_numastat(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_node_list(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);
extern int sysfs_do_cpu_kernel_max(sfsnode_t *snp, uio_t uio, vfs_context_t ctx);

#endif /* __FSBUNDLE__ */

#endif /* sysfs_h */
