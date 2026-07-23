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

#include <fs/sysfs/sysfs.h>
#include <fs/sysfs/sysfs_iokit.h>

#pragma mark -
#pragma mark External References

extern struct vfs_fsentry sysfs_vfsentry;
extern vfstable_t sysfs_vfs_table_ref;

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

    static int initialized;  // Protect against multiple calls.

    if (!initialized) {
        initialized = 1;

        // Create the tag for memory allocation.
        sysfs_osmalloc_tag = OSMalloc_Tagalloc(BUNDLEID_S, OSMT_DEFAULT);

        if (sysfs_osmalloc_tag == NULL) {
            return ENOMEM;   // Plausible error code.
        }

        // Allocate the lock group and the mutex lock for the hash table.
        sfsnode_lck_grp = lck_grp_alloc_init(SYSFS_LCKGRP_NAME, LCK_GRP_ATTR_NULL);
        sfsnode_hash_mutex = lck_mtx_alloc_init(sfsnode_lck_grp, LCK_ATTR_NULL);
    }

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
