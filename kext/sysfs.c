/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs.c
 *
 * This file contains the initialization and cleanup routines
 * as well as the start/stop routines for loading and unloading
 * the kernel extension.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>
#include <libkext.h>
#include <mach/kmod.h>
#include <mach/mach_types.h>
#include <os/log.h>
#include <sys/mount.h>

#include <sys/sysctl.h>

#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/sysfs_iokit.h>

#pragma mark -
#pragma mark External References

extern struct vfs_fsentry sysfs_vfsentry;
extern vfstable_t sysfs_vfs_table_ref;

#pragma mark -
#pragma mark Diagnostic sysctls

/*
 * Read-only counters for diagnosing runaway resource use while /sys is mounted:
 *
 *   sysctl sysfs
 *
 *     sysfs.snap_builds - full IORegistry snapshots built since load. Should be
 *                         nearly flat once the device tree settles; a number
 *                         that climbs steadily means rebuild churn.
 *     sysfs.snap_bytes  - memory held by live snapshots right now. Should sit at
 *                         roughly one snapshot's worth; steady growth means
 *                         snapshots are not being freed.
 *     sysfs.live_nodes  - sfsnode_t currently in the hash (one per live vnode).
 *                         Should plateau and fall back; unbounded growth means
 *                         vnodes are never reclaimed.
 *
 * Sample them a few times a minute apart with /sys mounted: whichever one grows
 * without bound identifies the subsystem at fault.
 */
extern int64_t sysfs_stat_snap_builds;
extern int64_t sysfs_stat_snap_bytes;
extern int64_t sysfs_stat_live_nodes;

/*
 * The oids are built by hand rather than with the SYSCTL_NODE / SYSCTL_QUAD
 * macros. Under XNU_KERNEL_PRIVATE (which this kext compiles with) those macros
 * expand to an in-kernel STARTUP auto-registration referencing
 * sysctl_register_oid_early() - an internal symbol NOT exported to kexts, so the
 * kext fails to bind and never loads ("could not find a kext which exports this
 * symbol"). Constructing the sysctl_oid structs directly and registering them
 * through the KPI sysctl_register_oid() avoids that symbol entirely; omitting
 * CTLFLAG_PERMANENT lets us remove them at unload. (Same fix as the procfs
 * sibling's procfs.linux oids.)
 */
static struct sysctl_oid_list sysfs_sysctl_children;

static struct sysctl_oid sysfs_sysctl_node = {
    .oid_parent  = &sysctl__children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_NODE | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &sysfs_sysctl_children,
    .oid_arg2    = 0,
    .oid_name    = "sysfs",
    .oid_handler = NULL,
    .oid_fmt     = "N",
    .oid_descr   = "sysfs filesystem",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid sysfs_sysctl_snap_builds = {
    .oid_parent  = &sysfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &sysfs_stat_snap_builds,
    .oid_arg2    = 0,
    .oid_name    = "snap_builds",
    .oid_handler = sysctl_handle_quad,
    .oid_fmt     = "Q",
    .oid_descr   = "IORegistry snapshots built since load",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid sysfs_sysctl_snap_bytes = {
    .oid_parent  = &sysfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &sysfs_stat_snap_bytes,
    .oid_arg2    = 0,
    .oid_name    = "snap_bytes",
    .oid_handler = sysctl_handle_quad,
    .oid_fmt     = "Q",
    .oid_descr   = "bytes held by live IORegistry snapshots",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid sysfs_sysctl_live_nodes = {
    .oid_parent  = &sysfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_QUAD | CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &sysfs_stat_live_nodes,
    .oid_arg2    = 0,
    .oid_name    = "live_nodes",
    .oid_handler = sysctl_handle_quad,
    .oid_fmt     = "Q",
    .oid_descr   = "sfsnodes currently in the node hash",
    .oid_version = SYSCTL_OID_VERSION,
};

STATIC void
sysfs_sysctl_register(void)
{
    sysctl_register_oid(&sysfs_sysctl_node);   /* parent first */
    sysctl_register_oid(&sysfs_sysctl_snap_builds);
    sysctl_register_oid(&sysfs_sysctl_snap_bytes);
    sysctl_register_oid(&sysfs_sysctl_live_nodes);
}

STATIC void
sysfs_sysctl_unregister(void)
{
    sysctl_unregister_oid(&sysfs_sysctl_live_nodes);
    sysctl_unregister_oid(&sysfs_sysctl_snap_bytes);
    sysctl_unregister_oid(&sysfs_sysctl_snap_builds);
    sysctl_unregister_oid(&sysfs_sysctl_node);
}

#pragma mark -
#pragma mark Initialization and finishing routines

/*
 * Initialization. Initializes static data, which is required when
 * the first mount occurs. Called only once during kernel startup,
 * but we interlock anyway to ensure that we don't perform intialization
 * more than once.
 */
int
sysfs_init(__unused struct vfsconf *vfsconf)
{
    int error = 0;
    static int initialized = 0; // Protect against multiple calls.

    if (initialized) {
        return 0;
    }

    // Create the tag for memory allocation.
    sysfs_osmalloc_tag = OSMalloc_Tagalloc(BUNDLEID_S, OSMT_DEFAULT);
    if (sysfs_osmalloc_tag == NULL) {
        return ENOMEM;
    }

    // Allocate the lock group attribute.
    lck_grp_attr_t *sfsnode_lck_grp_attr = lck_grp_attr_alloc_init();
    if (sfsnode_lck_grp_attr == NULL) {
        error = ENOMEM;
        goto fail_tag;
    }

    // Allocate the lock group using the custom attribute.
    sfsnode_lck_grp = lck_grp_alloc_init(SYSFS_LCKGRP_NAME, sfsnode_lck_grp_attr);

    // Free the attribute handle immediately; sfsnode_lck_grp is already initialized.
    lck_grp_attr_free(sfsnode_lck_grp_attr); 

    if (sfsnode_lck_grp == NULL) {
        error = ENOMEM;
        goto fail_tag;
    }

    // Allocate the mutex lock for the hash table.
    sfsnode_hash_mutex = lck_mtx_alloc_init(sfsnode_lck_grp, LCK_ATTR_NULL);
    if (sfsnode_hash_mutex == NULL) {
        error = ENOMEM;
        goto fail_grp;
    }

    initialized = 1;
    return 0;

/* Error Rollback Paths */
fail_grp:
    lck_grp_free(sfsnode_lck_grp);
    sfsnode_lck_grp = NULL;

fail_tag:
    OSMalloc_Tagfree(sysfs_osmalloc_tag);
    sysfs_osmalloc_tag = NULL;

    return error;
}

/*
 * Cleanup routine. Free the memory allocation tag, lock group and
 * hash mutex upon unloading the kext.
 */
int
sysfs_fini(void)
{
    int error = 0;

    if (sysfs_osmalloc_tag != NULL) {
        OSMalloc_Tagfree(sysfs_osmalloc_tag);
        sysfs_osmalloc_tag = NULL;
    }

    if (sfsnode_hash_mutex != NULL) {
        lck_mtx_free(sfsnode_hash_mutex, sfsnode_lck_grp);
        sfsnode_hash_mutex = NULL;
    }

    if (sfsnode_lck_grp != NULL) {
        lck_grp_free(sfsnode_lck_grp);
        sfsnode_lck_grp = NULL;
    }

    return error;
}

#pragma mark -
#pragma mark Start/Stop Routines

kern_return_t
sysfs_start(kmod_info_t *ki, __unused void *d)
{
    uuid_string_t uuid;
    struct vfsconf *vfsc = NULL;
    int ret = 0;

    os_log(OS_LOG_DEFAULT, "%s \n", version);     /* Print darwin kernel version */

    ret = libkext_vma_uuid(ki->address, uuid);
    kassert(ret == 0);

    os_log(OS_LOG_DEFAULT, "kext executable uuid %s \n", uuid);

    ret = sysfs_init(vfsc);
    if (ret != 0) {
        os_log(OS_LOG_DEFAULT, "sysfs_init() failed errno:  %d \n", ret);
        return KERN_FAILURE;
    }

    os_log(OS_LOG_DEFAULT, "lock group(%s) allocated \n", SYSFS_LCKGRP_NAME);

    ret = vfs_fsadd(&sysfs_vfsentry, &sysfs_vfs_table_ref);
    if (ret != 0) {
        os_log(OS_LOG_DEFAULT, "vfs_fsadd() failure  errno: %d \n", ret);
        sysfs_vfs_table_ref = NULL;
        sysfs_fini();
        return KERN_FAILURE;
    }

    os_log(OS_LOG_DEFAULT, "%s file system registered", sysfs_vfsentry.vfe_fsname);

    /*
     * Bring up the IOKit snapshot machinery (the lock; the snapshot itself is
     * built lazily on first use). Must happen before any mount.
     */
    sysfs_iokit_init();

    /*
     * Diagnostic counters (sysctl sysfs).
     */
    sysfs_sysctl_register();

    os_log(OS_LOG_DEFAULT, "loaded %s version %s build %s (%s) \n",
        BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);

    return KERN_SUCCESS;
}

kern_return_t
sysfs_stop(__unused kmod_info_t *ki, __unused void *d)
{
    uuid_string_t uuid;
    kern_return_t ret = 0;

    ret = libkext_vma_uuid(ki->address, uuid);
    if (ret != 0) {
        os_log(OS_LOG_DEFAULT, "util_vma_uuid() failed  errno: %d \n", ret);
        return KERN_FAILURE;
    }

    if (sysfs_vfs_table_ref != NULL) {
        ret = vfs_fsremove(sysfs_vfs_table_ref);
        if (ret != 0) {
            os_log(OS_LOG_DEFAULT, "vfs_fsremove() failure  errno: %d \n", ret);
            return KERN_FAILURE;
        }
    }

    /*
     * Release the IOKit snapshot and its lock (after the fs is unregistered, so
     * no more fs ops can reach it).
     */
    sysfs_sysctl_unregister();

    sysfs_iokit_teardown();

    sysfs_fini();
    libkext_massert();

    os_log(OS_LOG_DEFAULT, "unloaded %s version %s build %s (%s) \n",
        BUNDLEID_S, KEXTVERSION_S, KEXTBUILD_S, __TS__);

    return KERN_SUCCESS;
}

KMOD_EXPLICIT_DECL (BUNDLEID_S, KEXTBUILD_S, sysfs_start, sysfs_stop)
  __attribute__ ((visibility ("default")))

__private_extern__ kmod_start_func_t *_realmain = sysfs_start;
__private_extern__ kmod_start_func_t *_antimain = sysfs_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
