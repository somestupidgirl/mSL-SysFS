/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_iokit.cpp
 *
 * The kext's one C++ translation unit: it walks the IORegistry - macOS's device
 * model, which /sys/devices mirrors - through the in-kernel IOKit runtime and
 * exposes a small C-linkage surface (sysfs_iokit.h) that the C filesystem code
 * calls. Everything is done against the base IORegistryEntry / IOService KPIs
 * (traversal in gIOServicePlane, entry ids, names), so only com.apple.kpi.iokit
 * and com.apple.kpi.libkern are needed - both already declared in Info.plist,
 * and no MacKernelSDK.
 *
 * Each call resolves the registry entry from its IORegistryEntryID afresh
 * (id 0 == the registry root == the /sys/devices container), so no live IOKit
 * object is cached across calls - the registry can change under us at any time.
 *
 * NOTE on OSPtr: in a normal kext build OSPtr<T> is a plain T* (the shared-ptr
 * form is opt-in), so the returned iterators/services are raw pointers with the
 * usual manual retain/release, exactly as the procfs sibling does.
 */
#include <IOKit/IOService.h>

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/libkern.h>

#include <string.h>

#include <fs/sysfs/sysfs_iokit.h>

/* Upper bound on an IOKit entry name we copy onto a kernel stack. */
#define SYSFS_IOKIT_NAMEMAX 128

/*
 * Resolve an IORegistryEntryID to a retained registry entry, or nullptr. id 0 is
 * the registry root (retained here so every caller releases uniformly). Non-zero
 * ids are looked up in the service plane by entry-id matching. Caller releases.
 */
static IORegistryEntry *
sysfs_copy_entry(uint64_t regid)
{
    if (regid == 0) {
        IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
        if (root != nullptr) {
            root->retain();
        }
        return root;
    }

    OSDictionary *match = IOService::registryEntryIDMatching(regid);
    if (match == nullptr) {
        return nullptr;
    }
    /* copyMatchingService returns a retained service (or nullptr) and does not
     * consume `match`. */
    IOService *svc = IOService::copyMatchingService(match);
    match->release();
    return svc;   /* IOService is-a IORegistryEntry */
}

/*
 * Compose the unique display name of `entry`'s index-th child (in the service
 * plane) into buf, and its entry id into *child_id. Returns true if that child
 * exists. Uniqueness: the child gets its bare IOKit name unless an earlier
 * sibling already used that name, in which case an "@<n>" suffix (n = number of
 * earlier same-named siblings) is appended - deterministic for a stable
 * registry, so child_at and child_named always agree. `entry` is borrowed.
 */
static bool
sysfs_child_name_at(IORegistryEntry *entry, unsigned int index,
                    char *buf, size_t buflen, uint64_t *child_id)
{
    char base[SYSFS_IOKIT_NAMEMAX];
    uint64_t id = 0;

    /* Pass 1: locate the child at `index`; capture its base name and entry id. */
    OSIterator *it = entry->getChildIterator(gIOServicePlane);
    if (it == nullptr) {
        return false;
    }
    bool got = false;
    unsigned int i = 0;
    OSObject *obj;
    while ((obj = it->getNextObject()) != nullptr) {
        IORegistryEntry *c = OSDynamicCast(IORegistryEntry, obj);
        if (c == nullptr) {
            continue;   /* index only counts registry entries */
        }
        if (i == index) {
            const char *nm = c->getName(gIOServicePlane);
            strlcpy(base, nm != nullptr ? nm : "unknown", sizeof(base));
            id = c->getRegistryEntryID();
            got = true;
            break;
        }
        i++;
    }
    it->release();
    if (!got) {
        return false;
    }

    /* Pass 2: count earlier siblings [0, index) sharing this base name. */
    unsigned int dup = 0;
    it = entry->getChildIterator(gIOServicePlane);
    if (it != nullptr) {
        i = 0;
        while ((obj = it->getNextObject()) != nullptr && i < index) {
            IORegistryEntry *c = OSDynamicCast(IORegistryEntry, obj);
            if (c == nullptr) {
                continue;
            }
            const char *nm = c->getName(gIOServicePlane);
            if (nm != nullptr && strcmp(nm, base) == 0) {
                dup++;
            }
            i++;
        }
        it->release();
    }

    if (dup == 0) {
        strlcpy(buf, base, buflen);
    } else {
        snprintf(buf, buflen, "%s@%u", base, dup);
    }
    *child_id = id;
    return true;
}

extern "C" int
sysfs_iokit_entry_exists(uint64_t regid)
{
    if (regid == 0) {
        return 1;
    }
    IORegistryEntry *entry = sysfs_copy_entry(regid);
    if (entry != nullptr) {
        entry->release();
        return 1;
    }
    return 0;
}

extern "C" int
sysfs_iokit_child_at(uint64_t regid, unsigned int index,
                     char *namebuf, size_t buflen, uint64_t *child_regid)
{
    if (namebuf == nullptr || child_regid == nullptr || buflen == 0) {
        return 0;
    }
    IORegistryEntry *entry = sysfs_copy_entry(regid);
    if (entry == nullptr) {
        return 0;
    }
    bool ok = sysfs_child_name_at(entry, index, namebuf, buflen, child_regid);
    entry->release();
    return ok ? 1 : 0;
}

extern "C" int
sysfs_iokit_child_named(uint64_t regid, const char *name, size_t namelen,
                        uint64_t *child_regid)
{
    if (name == nullptr || child_regid == nullptr) {
        return 0;
    }
    IORegistryEntry *entry = sysfs_copy_entry(regid);
    if (entry == nullptr) {
        return 0;
    }

    char buf[SYSFS_IOKIT_NAMEMAX + 16];
    bool found = false;
    for (unsigned int i = 0; ; i++) {
        uint64_t id = 0;
        if (!sysfs_child_name_at(entry, i, buf, sizeof(buf), &id)) {
            break;   /* past the last child */
        }
        if (strlen(buf) == namelen && strncmp(buf, name, namelen) == 0) {
            *child_regid = id;
            found = true;
            break;
        }
    }
    entry->release();
    return found ? 1 : 0;
}

extern "C" int
sysfs_iokit_parent(uint64_t regid, uint64_t *parent_regid)
{
    if (parent_regid == nullptr) {
        return 0;
    }
    if (regid == 0) {
        *parent_regid = 0;   /* the devices root is its own top */
        return 1;
    }
    IORegistryEntry *entry = sysfs_copy_entry(regid);
    if (entry == nullptr) {
        return 0;
    }

    uint64_t pid = 0;
    IORegistryEntry *parent = entry->getParentEntry(gIOServicePlane);   /* borrowed */
    if (parent != nullptr && parent != IORegistryEntry::getRegistryRoot()) {
        pid = parent->getRegistryEntryID();
    }
    entry->release();

    *parent_regid = pid;
    return 1;
}

extern "C" size_t
sysfs_iokit_name(uint64_t regid, char *buf, size_t buflen)
{
    if (buf == nullptr || buflen == 0) {
        return 0;
    }
    IORegistryEntry *entry = sysfs_copy_entry(regid);
    if (entry == nullptr) {
        return 0;
    }

    size_t n = 0;
    const char *nm = entry->getName(gIOServicePlane);
    if (nm != nullptr) {
        strlcpy(buf, nm, buflen);
        n = strlen(buf);
    }
    entry->release();
    return n;
}
