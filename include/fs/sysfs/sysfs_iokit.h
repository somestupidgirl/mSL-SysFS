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

#ifdef __cplusplus
}
#endif

#endif /* _FS_SYSFS_SYSFS_IOKIT_H_ */
