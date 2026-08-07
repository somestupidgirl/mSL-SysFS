/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_iokit.h
 *
 * C-callable surface of the kext's one C++ translation unit (sysfs_iokit.cpp).
 * The IORegistry - macOS's device model, which /sys/devices mirrors - is only
 * reachable through the C++ IOKit runtime (registry traversal, entry ids and
 * property reads). These entry points let the C filesystem code walk the
 * service plane without touching C++: every call resolves a registry entry from
 * its IORegistryEntryID afresh (id 0 == the registry root == the /sys/devices
 * container), does its work, and drops the reference, so nothing here caches a
 * live IOKit object across calls.
 *
 * Booleans are plain int (1/0) to keep the C/C++ ABI trivial.
 */
#ifndef _FS_SYSFS_SYSFS_IOKIT_H_
#define _FS_SYSFS_SYSFS_IOKIT_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Allocate / free the IOKit snapshot machinery. sysfs_iokit_init() is called
 * once from the kext start routine (before any mount), sysfs_iokit_teardown()
 * from the stop routine (after the last unmount) to release the snapshot and its
 * lock.
 */
void sysfs_iokit_init(void);
void sysfs_iokit_teardown(void);

/*
 * Build the first registry snapshot synchronously. Call from the mount path
 * only: walking the registry is safe there (no VFS locks held), whereas doing it
 * from a vnop inverts the VFS/IOKit lock order and wedges the system.
 */
void sysfs_iokit_prime(void);

/*
 * 1 if a registry entry with this id currently exists (id 0, the devices root,
 * is always present).
 */
int sysfs_iokit_entry_exists(uint64_t regid);

/*
 * Fill namebuf with the display name of the index-th child (in gIOServicePlane)
 * of the entry `regid`, and *child_regid with that child's entry id. Returns 1
 * if the child exists, 0 past the end. Sibling names are made unique: the first
 * child of a given IOKit name keeps it, later ones get an "@<n>" suffix, so the
 * directory never lists two identical names.
 */
int sysfs_iokit_child_at(uint64_t regid, unsigned int index,
                         char *namebuf, size_t buflen, uint64_t *child_regid);

/*
 * Resolve a child by its (unique, suffix-disambiguated) display name under
 * `regid`; fill *child_regid. Returns 1 if found. Uses the same naming rule as
 * sysfs_iokit_child_at, so readdir and lookup always agree.
 */
int sysfs_iokit_child_named(uint64_t regid, const char *name, size_t namelen,
                            uint64_t *child_regid);

/*
 * Fill *parent_regid with the entry id of `regid`'s parent in gIOServicePlane,
 * or 0 when the parent is the registry root (i.e. `regid` is a top-level
 * /sys/devices entry). Returns 1 on success.
 */
int sysfs_iokit_parent(uint64_t regid, uint64_t *parent_regid);

/*
 * Copy the entry's IOKit name into buf (NUL-terminated). Returns the length
 * written (excluding the NUL), or 0 on failure. Backs the per-device `name`
 * attribute file.
 */
size_t sysfs_iokit_name(uint64_t regid, char *buf, size_t buflen);

/*
 * Device classes.
 *
 * Linux groups devices by class under /sys/class/<class>/<device>, where each
 * entry is a symlink into /sys/devices. IOKit has the same notion - a registry
 * entry's class - so membership is decided once, while the snapshot is built,
 * by testing each entry against the IOKit class that corresponds to the Linux
 * one. Only devices with a BSD name are listed, since that name is what Linux
 * calls the device (en0, disk0s1, ...) and what makes the entry addressable.
 */
#define SYSFS_CLASS_NONE   0
#define SYSFS_CLASS_NET    1   /* IONetworkInterface  -> class/net    */
#define SYSFS_CLASS_BLOCK  2   /* IOMedia             -> class/block  */
#define SYSFS_CLASS_TTY    3   /* IOSerialBSDClient   -> class/tty    */
#define SYSFS_CLASS_POWER  4   /* IOPMPowerSource     -> class/power_supply */
#define SYSFS_CLASS_COUNT  5

/*
 * Per-member attributes a class listing can filter on. SYSFS_CLASSF_WHOLE marks
 * a block device that is a whole disk rather than a partition, which is what
 * separates /sys/block (whole disks only, partitions nested under their disk in
 * /sys/devices) from /sys/class/block (every block device, flat) - the same
 * split Linux makes.
 */
#define SYSFS_CLASSF_WHOLE 0x1u

/*
 * Enumerate the members of a class. sysfs_iokit_class_child_at fills namebuf
 * with the index-th member's device name and *regid with its entry id, and
 * returns 1 while members remain. sysfs_iokit_class_child_named resolves one by
 * name. Both read the published snapshot, so they never touch IOKit directly.
 */
int sysfs_iokit_class_child_at(uint32_t class_id, uint32_t match_flags,
                               unsigned int index,
                               char *namebuf, size_t buflen, uint64_t *regid);
int sysfs_iokit_class_child_named(uint32_t class_id, uint32_t match_flags,
                                  const char *name,
                                  size_t namelen, uint64_t *regid);

/*
 * Build the path of an entry relative to the /sys/devices root, e.g.
 * "J813AP/arm-io@10F00000/usb-drd0". Returns the length written, or 0 if the
 * entry is unknown or the path does not fit. This is what a /sys/class symlink
 * resolves to, prefixed with the right number of "../" segments by the caller.
 */
size_t sysfs_iokit_path(uint64_t regid, char *buf, size_t buflen);

/*
 * Loaded kernel extensions, for /sys/module.
 *
 * The kext list is captured with the registry snapshot on the refresh thread,
 * never in a vnop: OSKext::copyLoadedKextInfo() takes the kext lock and builds
 * a dictionary describing every loaded kext, far too heavy - and too
 * lock-entangled - to do while holding VFS locks. These accessors only read the
 * published snapshot.
 *
 * load_tag is the kext's OSBundleLoadTag: unique among loaded kexts and stable
 * while the kext stays loaded, so it keys a /sys/module node the way
 * IORegistryEntryID keys a /sys/devices node.
 */
struct sysfs_module_info {
    uint64_t load_tag;
    uint64_t load_size;       /* OSBundleLoadSize, bytes */
    uint64_t refcnt;          /* OSBundleRetainCount */
    char     name[128];       /* CFBundleIdentifier */
    char     version[32];     /* CFBundleVersion */
};

/*
 * Fill *out with the index-th loaded kext; returns 1 while modules remain, 0
 * past the end. Enumeration order is whatever the kext list yields and is
 * stable for the life of one snapshot.
 */
int sysfs_iokit_module_at(unsigned int index, struct sysfs_module_info *out);

/* Resolve a module by bundle identifier. Returns 1 if found. */
int sysfs_iokit_module_named(const char *name, size_t namelen,
                             struct sysfs_module_info *out);

/* Resolve a module by its load tag (the per-node key). Returns 1 if found. */
int sysfs_iokit_module_by_tag(uint64_t load_tag, struct sysfs_module_info *out);

#ifdef __cplusplus
}
#endif

#endif /* _FS_SYSFS_SYSFS_IOKIT_H_ */
