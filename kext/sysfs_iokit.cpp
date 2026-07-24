/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * sysfs_iokit.cpp
 *
 * The kext's one C++ translation unit: it walks the IORegistry - macOS's device
 * model, which /sys/devices mirrors - through the in-kernel IOKit runtime and
 * exposes a small C-linkage surface (sysfs_iokit.h) that the C filesystem code
 * calls. Only com.apple.kpi.iokit and com.apple.kpi.libkern are needed (both in
 * Info.plist), and no MacKernelSDK.
 *
 * PERFORMANCE (this is what keeps /sys mountable at the root without hanging
 * login): resolving a registry entry from its IORegistryEntryID has no O(1) KPI
 * - it is a registry-wide IOService match. Doing that per node made a File
 * Manager tree-walk of /sys/devices O(N^2), which pegged coreservicesd while it
 * built its volume "universe" at login and black-screened the machine. Instead
 * we build a SNAPSHOT of the whole service plane once (a flat node table with a
 * regid->index hash and CSR child lists) and answer every query from it in
 * O(1)/O(children). The snapshot is rebuilt on a short TTL, so it stays cheap
 * and self-consistent for the duration of a walk. The snapshot is a tree (each
 * entry under its first service-plane parent), so multiple-parent DAG links do
 * not duplicate subtrees or create cycles.
 */
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/libkern.h>

#include <string.h>

#include <fs/sysfs/sysfs_iokit.h>

extern "C" {
    void clock_get_uptime(uint64_t *result);
    void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
}

/* Upper bound on an IOKit entry name we store. */
#define SYSFS_IOKIT_NAMEMAX   128
/* Sanity cap on service-plane entries in one snapshot. */
#define SYSFS_IOKIT_MAXNODES  16384u
/* How long a snapshot is reused before a rebuild (ns). */
#define SYSFS_SNAP_TTL_NS     2000000000ULL   /* 2 seconds */

#define SYSFS_IDX_NONE        0xFFFFFFFFu

/* One node in the snapshot. Node 0 is the synthetic root (regid 0 == the
 * /sys/devices container); nodes 1.. are real service-plane entries. */
struct sysfs_snap_node {
    uint64_t regid;
    uint64_t parent_regid;    /* 0 == root */
    uint32_t first_child;     /* index into g_child_idx */
    uint32_t num_children;
    char     name[SYSFS_IOKIT_NAMEMAX];   /* base IOKit name */
};

/* Open-addressing hash slot: regid -> node index. */
struct sysfs_hash_slot {
    uint64_t regid;
    uint32_t index;           /* SYSFS_IDX_NONE == empty */
};

static IOLock                 *g_lock       = nullptr;
static struct sysfs_snap_node *g_nodes      = nullptr;
static uint32_t               *g_child_idx  = nullptr;
static struct sysfs_hash_slot *g_hash       = nullptr;
static uint32_t                g_node_count = 0;   /* used nodes (incl. root) */
static uint32_t                g_node_cap   = 0;   /* allocated nodes */
static uint32_t                g_child_cap  = 0;   /* allocated child_idx */
static uint32_t                g_hash_size  = 0;   /* power of two */
static uint64_t                g_snap_uptime = 0;
static bool                    g_snap_valid  = false;

#pragma mark - allocation helpers

static void *
sysfs_alloc(size_t n)
{
    void *p = IOMalloc(n);
    if (p != nullptr) {
        bzero(p, n);
    }
    return p;
}

static void
sysfs_free(void *addr, size_t len)
{
    if (addr != nullptr && len > 0) {
        IOFree(addr, len);
    }
}

static void
sysfs_snap_free(void)
{
    if (g_nodes != nullptr) {
        sysfs_free(g_nodes, (size_t)g_node_cap * sizeof(*g_nodes));
        g_nodes = nullptr;
    }
    if (g_child_idx != nullptr) {
        sysfs_free(g_child_idx, (size_t)g_child_cap * sizeof(*g_child_idx));
        g_child_idx = nullptr;
    }
    if (g_hash != nullptr) {
        sysfs_free(g_hash, (size_t)g_hash_size * sizeof(*g_hash));
        g_hash = nullptr;
    }
    g_node_count = g_node_cap = g_child_cap = g_hash_size = 0;
    g_snap_valid = false;
}

#pragma mark - hash (regid -> node index)

static uint32_t
sysfs_hash_of(uint64_t regid)
{
    /* Fibonacci-ish mix; masked to the table size by the caller. */
    return (uint32_t)((regid * 0x9E3779B97F4A7C15ULL) >> 40);
}

static void
sysfs_hash_insert(uint64_t regid, uint32_t index)
{
    uint32_t mask = g_hash_size - 1;
    uint32_t h = sysfs_hash_of(regid) & mask;
    for (uint32_t n = 0; n < g_hash_size; n++) {
        if (g_hash[h].index == SYSFS_IDX_NONE) {
            g_hash[h].regid = regid;
            g_hash[h].index = index;
            return;
        }
        if (g_hash[h].index != SYSFS_IDX_NONE && g_hash[h].regid == regid) {
            return;   /* already present (dedup) */
        }
        h = (h + 1) & mask;
    }
}

/* Returns node index for regid, or SYSFS_IDX_NONE. */
static uint32_t
sysfs_hash_find(uint64_t regid)
{
    if (g_hash == nullptr || g_hash_size == 0) {
        return SYSFS_IDX_NONE;
    }
    uint32_t mask = g_hash_size - 1;
    uint32_t h = sysfs_hash_of(regid) & mask;
    for (uint32_t n = 0; n < g_hash_size; n++) {
        if (g_hash[h].index == SYSFS_IDX_NONE) {
            return SYSFS_IDX_NONE;
        }
        if (g_hash[h].regid == regid) {
            return g_hash[h].index;
        }
        h = (h + 1) & mask;
    }
    return SYSFS_IDX_NONE;
}

#pragma mark - snapshot build

/* Count service-plane entries (one recursive pass). */
static uint32_t
sysfs_count_entries(void)
{
    IORegistryIterator *it = IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (it == nullptr) {
        return 0;
    }
    uint32_t n = 0;
    IORegistryEntry *e;
    while ((e = it->getNextObject()) != nullptr && n < SYSFS_IOKIT_MAXNODES) {
        n++;
    }
    it->release();
    return n;
}

/* (Re)build the snapshot. Caller holds g_lock. */
static void
sysfs_snap_build(void)
{
    sysfs_snap_free();

    uint32_t maxe = sysfs_count_entries();          /* upper bound (dups possible) */
    uint32_t buffer = (maxe / 10) + 32;             /* Small adaptive buffer for dynamic nodes attached mid-scan */
    uint32_t cap = maxe + buffer + 1;               /* +1 for synthetic root */

    /* Power-of-two hash table, >= 2x capacity, min 16. */
    uint32_t hsize = 16;
    while (hsize < cap * 2u) {
        hsize <<= 1;
    }

    g_nodes     = (struct sysfs_snap_node *)sysfs_alloc((size_t)cap * sizeof(*g_nodes));
    g_child_idx = (uint32_t *)sysfs_alloc((size_t)cap * sizeof(*g_child_idx));
    g_hash      = (struct sysfs_hash_slot *)sysfs_alloc((size_t)hsize * sizeof(*g_hash));
    if (g_nodes == nullptr || g_child_idx == nullptr || g_hash == nullptr) {
        sysfs_snap_free();
        return;
    }
    g_node_cap  = cap;
    g_child_cap = cap;
    g_hash_size = hsize;
    for (uint32_t i = 0; i < hsize; i++) {
        g_hash[i].index = SYSFS_IDX_NONE;
    }

    /* Node 0: the synthetic root / devices container. */
    g_nodes[0].regid = 0;
    g_nodes[0].parent_regid = 0;
    g_nodes[0].name[0] = '\0';
    sysfs_hash_insert(0, 0);
    uint32_t count = 1;

    IORegistryEntry *root = IORegistryEntry::getRegistryRoot();
    IORegistryIterator *it =
        IORegistryIterator::iterateOver(gIOServicePlane, kIORegistryIterateRecursively);
    if (it != nullptr) {
        IORegistryEntry *e;
        while ((e = it->getNextObject()) != nullptr && count < cap) {
            uint64_t regid = e->getRegistryEntryID();
            if (regid == 0) {
                continue;   /* reserved for the root sentinel */
            }
            if (sysfs_hash_find(regid) != SYSFS_IDX_NONE) {
                continue;   /* DAG duplicate - keep the first appearance */
            }

            IORegistryEntry *parent = e->getParentEntry(gIOServicePlane);
            uint64_t pregid = 0;
            if (parent != nullptr) {
                pregid = (parent == root) ? 0 : parent->getRegistryEntryID();
                parent->release(); /* Release the reference returned by getParentEntry */
            }

            struct sysfs_snap_node *nd = &g_nodes[count];
            nd->regid = regid;
            nd->parent_regid = pregid;
            nd->first_child = 0;
            nd->num_children = 0;
            const char *nm = e->getName(gIOServicePlane);
            strlcpy(nd->name, nm != nullptr ? nm : "unknown", sizeof(nd->name));
            sysfs_hash_insert(regid, count);
            count++;
        }
        it->release();
    }
    g_node_count = count;

    /* Build CSR child lists: count, prefix-sum, fill. A node's parent maps to a
     * node index via the hash (parent_regid 0 -> root at index 0); an orphan
     * whose parent was not captured is attached to the root. */
    for (uint32_t i = 1; i < count; i++) {
        uint32_t p = sysfs_hash_find(g_nodes[i].parent_regid);
        if (p == SYSFS_IDX_NONE) {
            p = 0;
        }
        g_nodes[p].num_children++;
    }
    uint32_t off = 0;
    for (uint32_t j = 0; j < count; j++) {
        g_nodes[j].first_child = off;
        off += g_nodes[j].num_children;
    }
    /* Temporary fill cursors (reuse a small alloc). */
    uint32_t *cursor = (uint32_t *)sysfs_alloc((size_t)count * sizeof(uint32_t));
    if (cursor == nullptr) {
        /* Without cursors we cannot place children; drop to an empty tree. */
        for (uint32_t j = 0; j < count; j++) {
            g_nodes[j].num_children = 0;
        }
    } else {
        for (uint32_t j = 0; j < count; j++) {
            cursor[j] = g_nodes[j].first_child;
        }
        for (uint32_t i = 1; i < count; i++) {
            uint32_t p = sysfs_hash_find(g_nodes[i].parent_regid);
            if (p == SYSFS_IDX_NONE) {
                p = 0;
            }
            g_child_idx[cursor[p]++] = i;
        }
        sysfs_free(cursor, (size_t)count * sizeof(uint32_t));
    }

    clock_get_uptime(&g_snap_uptime);
    g_snap_valid = true;
}

/* Ensure a fresh-enough snapshot exists. Caller holds g_lock. */
static void
sysfs_snap_ensure(void)
{
    if (g_snap_valid && g_nodes != nullptr) {
        uint64_t now, elapsed_ns;
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - g_snap_uptime, &elapsed_ns);
        if (elapsed_ns <= SYSFS_SNAP_TTL_NS) {
            return;
        }
    }
    sysfs_snap_build();
}

#pragma mark - display names

/* Compose the unique display name of the index-th child of node `pidx` into buf
 * (base IOKit name, plus an "@<n>" suffix if earlier siblings share the base
 * name), and its regid into *child_regid. Returns true if that child exists.
 * Caller holds g_lock. */
static bool
sysfs_child_display(uint32_t pidx, uint32_t index, char *buf, size_t buflen,
                    uint64_t *child_regid)
{
    if (pidx >= g_node_count || index >= g_nodes[pidx].num_children) {
        return false;
    }
    uint32_t base = g_nodes[pidx].first_child;
    uint32_t ci = g_child_idx[base + index];
    const char *nm = g_nodes[ci].name;

    uint32_t dup = 0;
    for (uint32_t k = 0; k < index; k++) {
        uint32_t sib = g_child_idx[base + k];
        if (strcmp(g_nodes[sib].name, nm) == 0) {
            dup++;
        }
    }
    if (dup == 0) {
        strlcpy(buf, nm, buflen);
    } else {
        snprintf(buf, buflen, "%s@%u", nm, dup);
    }
    *child_regid = g_nodes[ci].regid;
    return true;
}

#pragma mark - C API

extern "C" void
sysfs_iokit_init(void)
{
    if (g_lock == nullptr) {
        g_lock = IOLockAlloc();
    }
}

extern "C" void
sysfs_iokit_teardown(void)
{
    if (g_lock != nullptr) {
        IOLockLock(g_lock);
        sysfs_snap_free();
        IOLockUnlock(g_lock);
        IOLockFree(g_lock);
        g_lock = nullptr;
    }
}

extern "C" int
sysfs_iokit_entry_exists(uint64_t regid)
{
    if (regid == 0) {
        return 1;
    }
    if (g_lock == nullptr) {
        return 0;
    }
    IOLockLock(g_lock);
    sysfs_snap_ensure();
    int found = (sysfs_hash_find(regid) != SYSFS_IDX_NONE) ? 1 : 0;
    IOLockUnlock(g_lock);
    return found;
}

extern "C" int
sysfs_iokit_child_at(uint64_t regid, unsigned int index,
                     char *namebuf, size_t buflen, uint64_t *child_regid)
{
    if (namebuf == nullptr || child_regid == nullptr || buflen == 0 || g_lock == nullptr) {
        return 0;
    }
    IOLockLock(g_lock);
    sysfs_snap_ensure();
    uint32_t pidx = sysfs_hash_find(regid);
    int ok = 0;
    if (pidx != SYSFS_IDX_NONE) {
        ok = sysfs_child_display(pidx, (uint32_t)index, namebuf, buflen, child_regid) ? 1 : 0;
    }
    IOLockUnlock(g_lock);
    return ok;
}

extern "C" int
sysfs_iokit_child_named(uint64_t regid, const char *name, size_t namelen,
                        uint64_t *child_regid)
{
    if (name == nullptr || child_regid == nullptr || g_lock == nullptr) {
        return 0;
    }
    IOLockLock(g_lock);
    sysfs_snap_ensure();
    int found = 0;
    uint32_t pidx = sysfs_hash_find(regid);
    if (pidx != SYSFS_IDX_NONE) {
        char buf[SYSFS_IOKIT_NAMEMAX + 16];
        uint32_t n = g_nodes[pidx].num_children;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t id = 0;
            if (!sysfs_child_display(pidx, i, buf, sizeof(buf), &id)) {
                break;
            }
            if (strlen(buf) == namelen && strncmp(buf, name, namelen) == 0) {
                *child_regid = id;
                found = 1;
                break;
            }
        }
    }
    IOLockUnlock(g_lock);
    return found;
}

extern "C" int
sysfs_iokit_parent(uint64_t regid, uint64_t *parent_regid)
{
    if (parent_regid == nullptr) {
        return 0;
    }
    if (regid == 0) {
        *parent_regid = 0;
        return 1;
    }
    if (g_lock == nullptr) {
        return 0;
    }
    IOLockLock(g_lock);
    sysfs_snap_ensure();
    int ok = 0;
    uint32_t idx = sysfs_hash_find(regid);
    if (idx != SYSFS_IDX_NONE) {
        *parent_regid = g_nodes[idx].parent_regid;
        ok = 1;
    }
    IOLockUnlock(g_lock);
    return ok;
}

extern "C" size_t
sysfs_iokit_name(uint64_t regid, char *buf, size_t buflen)
{
    if (buf == nullptr || buflen == 0 || g_lock == nullptr) {
        return 0;
    }
    IOLockLock(g_lock);
    sysfs_snap_ensure();
    size_t n = 0;
    uint32_t idx = sysfs_hash_find(regid);
    if (idx != SYSFS_IDX_NONE) {
        strlcpy(buf, g_nodes[idx].name, buflen);
        n = strlen(buf);
    }
    IOLockUnlock(g_lock);
    return n;
}
