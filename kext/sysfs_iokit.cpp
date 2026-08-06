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

#include <kern/thread_call.h>

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

/*
 * Hard floor between two snapshot rebuilds, whatever the registry generation
 * says. A rebuild walks the entire service plane, and every step of that walk
 * takes IOKit's registry locks - the same locks driver matching, power
 * management and HID input delivery need. Shortly after login the generation
 * count changes constantly as drivers attach, so gating rebuilds purely on the
 * generation meant a fresh full traversal on nearly every touch of /sys. That
 * surfaced as short system-wide stalls: apps slow to launch, builds not
 * finishing, even keyboard input hitching. The device tree does not change
 * interestingly enough to justify re-reading it that often.
 */
#define SYSFS_SNAP_MIN_REBUILD_NS 30000000000ULL  /* 30 seconds */

#define SYSFS_IDX_NONE        0xFFFFFFFFu

/* One node in the snapshot. Node 0 is the synthetic root (regid 0 == the
 * /sys/devices container); nodes 1.. are real service-plane entries. */
struct sysfs_snap_node {
    uint64_t regid;
    uint64_t parent_regid;    /* 0 == root */
    uint32_t first_child;     /* index into g_child_idx */
    uint32_t num_children;
    /*
     * How many earlier siblings share this node's IOKit name: 0 means the name
     * is unique among its siblings and is used bare, N>0 means it is presented
     * as "<name>@N". Computed once when the snapshot is built (below) rather
     * than per query - working it out on demand meant every child_at() and
     * child_named() rescanned all earlier siblings, making a readdir or a path
     * lookup O(children^2) in strcmp with the snapshot lock held. On the wide
     * directories the IORegistry actually has, that lock hold was long enough to
     * stall every other process touching /sys.
     */
    uint32_t dup_ordinal;
    char     name[SYSFS_IOKIT_NAMEMAX];   /* base IOKit name */
};

/* Open-addressing hash slot: regid -> node index. */
struct sysfs_hash_slot {
    uint64_t regid;
    uint32_t index;           /* SYSFS_IDX_NONE == empty */
};

/* Container holding a full self-contained IOKit registry snapshot */
struct sysfs_snapshot {
    struct sysfs_snap_node *nodes;
    uint32_t               *child_idx;
    struct sysfs_hash_slot *hash;
    uint32_t                node_count;
    uint32_t                node_cap;
    uint32_t                child_cap;
    uint32_t                hash_size;
    uint64_t                uptime;
    size_t                  bytes;    /* total allocation, for sysfs_stat_snap_bytes */
};

static IOLock                 *g_lock       = nullptr;
static struct sysfs_snapshot  *g_snapshot    = nullptr;

/*
 * Rebuild throttling. Building a snapshot is expensive (two full recursive
 * service-plane traversals plus ~1MB of IOMalloc), so it must happen as rarely
 * as possible and never more than once at a time:
 *
 *   g_building  - single-flight guard. Without it every concurrent caller that
 *                 sees a stale snapshot builds its own and throws all but one
 *                 away. Under a multi-threaded walk (Spotlight, File Manager,
 *                 find) that is dozens of simultaneous full traversals, and as
 *                 they slow each other down the build time eventually exceeds
 *                 the TTL - at which point every published snapshot is already
 *                 stale and the storm becomes self-sustaining, degrading the
 *                 machine over minutes until it stops responding.
 *   g_snap_gen  - the IORegistry generation the snapshot was built from. The
 *                 registry topology only changes when hardware/drivers come and
 *                 go, so if the generation is unchanged the snapshot is still
 *                 accurate and we skip the rebuild entirely regardless of age.
 *                 This turns steady-state operation from "rebuild every TTL
 *                 forever" into "rebuild only when the device tree changes".
 */
static bool                    g_building    = false;
static SInt32                  g_snap_gen    = 0;
static bool                    g_snap_gen_valid = false;

/* Uptime at which the last build completed, for the rate limit above. */
static uint64_t                g_last_build_uptime = 0;
static bool                    g_last_build_valid  = false;

/*
 * Node count of the last snapshot, used to size the next one. Sizing from the
 * previous result removes an entire second traversal per rebuild: the build used
 * to walk the whole service plane once purely to count it, then again to record
 * it, doubling the registry-lock traffic to learn a number the previous
 * snapshot already knew.
 */
static uint32_t                g_last_node_count = 0;

/*
 * Asynchronous refresh.
 *
 * A vnop should not walk the IORegistry itself. A filesystem operation runs with
 * VFS locks held (and a vnode iocount), while IORegistryIterator takes the
 * global IOKit registry lock, which driver matching, power management and I/O
 * completion all contend for. Taking a heavily contended global lock underneath
 * VFS locks risks stalling threads while they hold vnodes, which in turn stalls
 * any unmount (vflush() must reclaim every vnode). This was not the cause of the
 * unmount freeze - that was heap corruption in release_node() - but keeping the
 * traversal off the vnop path is the right structure regardless, and it is what
 * makes the single-flight/generation throttling above meaningful.
 *
 * So the registry walk happens only in safe contexts: synchronously at mount
 * (sysfs_iokit_prime), and thereafter on a thread_call. Vnops just read whatever
 * snapshot is published - never allocating, never calling IOKit, never sleeping.
 * A slightly stale snapshot is always preferable to blocking a vnop.
 */
static thread_call_t           g_refresh_call = nullptr;
static bool                    g_refresh_pending = false;

/*
 * Diagnostic counters, exposed read-only via the sysfs.* sysctls (see sysfs.c).
 * These exist to tell rebuild churn apart from a genuine allocation leak without
 * having to guess: snap_builds is how many full snapshots have been built (it
 * should stay nearly flat once the device tree settles), and snap_bytes is how
 * much memory live snapshots currently hold (it should stay at roughly one
 * snapshot's worth - continuous growth means snapshots are not being freed).
 */
extern "C" {
    int64_t sysfs_stat_snap_builds = 0;
    int64_t sysfs_stat_snap_bytes  = 0;
}

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
sysfs_snap_free(struct sysfs_snapshot *snap)
{
    if (snap == nullptr) {
        return;
    }
    if (snap->nodes != nullptr) {
        sysfs_free(snap->nodes, (size_t)snap->node_cap * sizeof(*snap->nodes));
    }
    if (snap->child_idx != nullptr) {
        sysfs_free(snap->child_idx, (size_t)snap->child_cap * sizeof(*snap->child_idx));
    }
    if (snap->hash != nullptr) {
        sysfs_free(snap->hash, (size_t)snap->hash_size * sizeof(*snap->hash));
    }
    if (snap->bytes != 0) {
        OSAddAtomic64(-(int64_t)snap->bytes, &sysfs_stat_snap_bytes);
    }
    sysfs_free(snap, sizeof(*snap));
}

#pragma mark - hash (regid -> node index)

static uint32_t
sysfs_hash_of(uint64_t regid)
{
    /* Fibonacci-ish mix; masked to the table size by the caller. */
    return (uint32_t)((regid * 0x9E3779B97F4A7C15ULL) >> 40);
}

static void
sysfs_hash_insert(struct sysfs_snapshot *snap, uint64_t regid, uint32_t index)
{
    uint32_t mask = snap->hash_size - 1;
    uint32_t h = sysfs_hash_of(regid) & mask;
    for (uint32_t n = 0; n < snap->hash_size; n++) {
        if (snap->hash[h].index == SYSFS_IDX_NONE) {
            snap->hash[h].regid = regid;
            snap->hash[h].index = index;
            return;
        }
        if (snap->hash[h].index != SYSFS_IDX_NONE && snap->hash[h].regid == regid) {
            return;   /* already present (dedup) */
        }
        h = (h + 1) & mask;
    }
}

/* Returns node index for regid, or SYSFS_IDX_NONE. */
static uint32_t
sysfs_hash_find(const struct sysfs_snapshot *snap, uint64_t regid)
{
    if (snap == nullptr || snap->hash == nullptr || snap->hash_size == 0) {
        return SYSFS_IDX_NONE;
    }
    uint32_t mask = snap->hash_size - 1;
    uint32_t h = sysfs_hash_of(regid) & mask;
    for (uint32_t n = 0; n < snap->hash_size; n++) {
        if (snap->hash[h].index == SYSFS_IDX_NONE) {
            return SYSFS_IDX_NONE;
        }
        if (snap->hash[h].regid == regid) {
            return snap->hash[h].index;
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


/* Builds a fresh snapshot completely in isolation without holding global locks. */
static struct sysfs_snapshot *
sysfs_snap_build(void)
{
    struct sysfs_snapshot *snap = (struct sysfs_snapshot *)sysfs_alloc(sizeof(*snap));
    if (snap == nullptr) {
        return nullptr;
    }

    /*
     * Size from the previous snapshot rather than counting the registry again.
     * The count was a second full recursive traversal - as expensive as the
     * build itself - to learn a number the last snapshot already knew, so only
     * the very first build pays for it. The margin absorbs devices attaching
     * between builds; if it is ever not enough the walk below stops at the cap
     * and the next rebuild starts from a doubled estimate, so a burst of new
     * devices costs freshness briefly rather than truncating for good.
     */
    uint32_t maxe = (g_last_node_count != 0)
        ? g_last_node_count
        : sysfs_count_entries();
    uint32_t buffer = (maxe / 4) + 64;              /* room for mid-scan attachments */
    uint32_t cap = maxe + buffer + 1;               /* +1 for synthetic root */
    if (cap > SYSFS_IOKIT_MAXNODES) {
        cap = SYSFS_IOKIT_MAXNODES;
    }

    /* Power-of-two hash table, >= 2x capacity, min 16. */
    uint32_t hsize = 16;
    while (hsize < cap * 2u) {
        hsize <<= 1;
    }

    snap->nodes     = (struct sysfs_snap_node *)sysfs_alloc((size_t)cap * sizeof(*snap->nodes));
    snap->child_idx = (uint32_t *)sysfs_alloc((size_t)cap * sizeof(*snap->child_idx));
    snap->hash      = (struct sysfs_hash_slot *)sysfs_alloc((size_t)hsize * sizeof(*snap->hash));
    if (snap->nodes == nullptr || snap->child_idx == nullptr || snap->hash == nullptr) {
        sysfs_snap_free(snap);
        return nullptr;
    }
    snap->node_cap  = cap;
    snap->child_cap = cap;
    snap->hash_size = hsize;
    snap->bytes = sizeof(*snap)
                + (size_t)cap * sizeof(*snap->nodes)
                + (size_t)cap * sizeof(*snap->child_idx)
                + (size_t)hsize * sizeof(*snap->hash);
    OSAddAtomic64((int64_t)snap->bytes, &sysfs_stat_snap_bytes);
    OSAddAtomic64(1, &sysfs_stat_snap_builds);
    for (uint32_t i = 0; i < hsize; i++) {
        snap->hash[i].index = SYSFS_IDX_NONE;
    }

    /* Node 0: the synthetic root / devices container. */
    snap->nodes[0].regid = 0;
    snap->nodes[0].parent_regid = 0;
    snap->nodes[0].name[0] = '\0';
    sysfs_hash_insert(snap, 0, 0);
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
            if (sysfs_hash_find(snap, regid) != SYSFS_IDX_NONE) {
                continue;   /* DAG duplicate - keep the first appearance */
            }

            IORegistryEntry *parent = e->getParentEntry(gIOServicePlane);
            uint64_t pregid = 0;
            if (parent != nullptr) {
                pregid = (parent == root) ? 0 : parent->getRegistryEntryID();
            }

            struct sysfs_snap_node *nd = &snap->nodes[count];
            nd->regid = regid;
            nd->parent_regid = pregid;
            nd->first_child = 0;
            nd->num_children = 0;
            const char *nm = e->getName(gIOServicePlane);
            strlcpy(nd->name, nm != nullptr ? nm : "unknown", sizeof(nd->name));
            sysfs_hash_insert(snap, regid, count);
            count++;
        }
        it->release();
    }
    snap->node_count = count;

    /*
     * If the walk filled the table the estimate was too small, so this snapshot
     * is truncated. Grow the estimate and let the next rebuild pick up the rest
     * rather than staying short forever.
     */
    if (count >= cap && cap < SYSFS_IOKIT_MAXNODES) {
        uint32_t grown = cap * 2;
        g_last_node_count = (grown > SYSFS_IOKIT_MAXNODES) ? SYSFS_IOKIT_MAXNODES : grown;
    }

    /* Build CSR child lists: count, prefix-sum, fill. A node's parent maps to a
     * node index via the hash (parent_regid 0 -> root at index 0); an orphan
     * whose parent was not captured is attached to the root.
     */
    uint64_t *child_counts =
        (uint64_t *)sysfs_alloc((size_t)count * sizeof(uint64_t));
    if (child_counts == nullptr) {
        /* Cannot safely build child lists; fall back to empty tree. */
        for (uint32_t j = 0; j < count; j++) {
            snap->nodes[j].num_children = 0;
            snap->nodes[j].first_child  = 0;
        }
        clock_get_uptime(&snap->uptime);
        return snap;
    }

    /* Count children per parent. */
    for (uint32_t i = 1; i < count; i++) {
        uint32_t p = sysfs_hash_find(snap, snap->nodes[i].parent_regid);
        if (p == SYSFS_IDX_NONE) {
            p = 0;
        }
        child_counts[p]++;
    }

    /* Validate total child count fits in allocated child_idx array. */
    uint64_t total_children = 0;
    for (uint32_t j = 0; j < count; j++) {
        total_children += child_counts[j];
    }
    if (total_children > snap->child_cap) {
        /* Registry mutated too much mid-scan; abort safely. */
        sysfs_free(child_counts, (size_t)count * sizeof(uint64_t));
        sysfs_snap_free(snap);
        return nullptr;
    }

    /* Assign num_children and prefix-sum first_child offsets. */
    uint32_t off = 0;
    for (uint32_t j = 0; j < count; j++) {
        snap->nodes[j].num_children = (uint32_t)child_counts[j];
        snap->nodes[j].first_child  = off;
        off += snap->nodes[j].num_children;
    }

    /* Fill child_idx using temporary cursors. */
    uint32_t *cursor =
        (uint32_t *)sysfs_alloc((size_t)count * sizeof(uint32_t));
    if (cursor == nullptr) {
        /* Cannot place children; degrade to empty tree. */
        for (uint32_t j = 0; j < count; j++) {
            snap->nodes[j].num_children = 0;
        }
    } else {
        for (uint32_t j = 0; j < count; j++) {
            cursor[j] = snap->nodes[j].first_child;
        }
        for (uint32_t i = 1; i < count; i++) {
            uint32_t p = sysfs_hash_find(snap, snap->nodes[i].parent_regid);
            if (p == SYSFS_IDX_NONE) {
                p = 0;
            }
            snap->child_idx[cursor[p]++] = i;
        }
        sysfs_free(cursor, (size_t)count * sizeof(uint32_t));
    }

    sysfs_free(child_counts, (size_t)count * sizeof(uint64_t));

    /*
     * Precompute each child's name-disambiguation ordinal. This is the same
     * O(siblings^2) comparison as before, but it now happens exactly once, here
     * on the refresh path, instead of on every lookup and every readdir entry
     * with the lock held.
     */
    for (uint32_t p = 0; p < count; p++) {
        uint32_t base = snap->nodes[p].first_child;
        uint32_t n    = snap->nodes[p].num_children;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t ci = snap->child_idx[base + i];
            uint32_t dup = 0;
            for (uint32_t k = 0; k < i; k++) {
                uint32_t sib = snap->child_idx[base + k];
                if (strcmp(snap->nodes[sib].name, snap->nodes[ci].name) == 0) {
                    dup++;
                }
            }
            snap->nodes[ci].dup_ordinal = dup;
        }
    }

    clock_get_uptime(&snap->uptime);
    return snap;
}

/*
 * Ensure a usable snapshot exists, rebuilding only when it is genuinely needed
 * and only ever on one thread at a time (see g_building / g_snap_gen above).
 *
 * Readers dereference g_snapshot only while holding g_lock, and a replaced
 * snapshot is freed after the lock is dropped - by which point it is no longer
 * reachable and no reader can still be inside the lock holding it.
 */
/*
 * Do one registry walk and publish the result. Must only be called from a
 * context that holds no VFS locks: the mount path, or the thread_call below.
 */
static void
sysfs_snap_refresh(void)
{
    /* Sampled outside the lock: a plain read of a global counter. */
    SInt32 gen = IORegistryEntry::getGenerationCount();

    struct sysfs_snapshot *new_snap = sysfs_snap_build();

    struct sysfs_snapshot *old_snap = nullptr;
    IOLockLock(g_lock);
    if (new_snap != nullptr) {
        old_snap = g_snapshot;
        g_snapshot = new_snap;
        /*
         * Record the generation sampled BEFORE the traversal: if the registry
         * changed while we were walking it, this snapshot may already be
         * incomplete, and the mismatch makes the next check schedule another
         * refresh.
         */
        g_snap_gen = gen;
        g_snap_gen_valid = true;
        g_last_node_count = new_snap->node_count;
        clock_get_uptime(&g_last_build_uptime);
        g_last_build_valid = true;
    }
    g_building = false;
    g_refresh_pending = false;
    /* Wake a teardown that is waiting for this refresh to drain. */
    IOLockWakeup(g_lock, &g_building, false);
    IOLockUnlock(g_lock);

    /* Free the replaced snapshot outside the lock. */
    sysfs_snap_free(old_snap);
}

/* thread_call entry point: the only place a refresh happens after mount. */
static void
sysfs_snap_refresh_cb(thread_call_param_t p0, thread_call_param_t p1)
{
    (void)p0;
    (void)p1;
    sysfs_snap_refresh();
}

/*
 * Called from vnops. Strictly non-blocking: it never walks the registry, never
 * allocates and never sleeps. If the published snapshot has gone stale it just
 * schedules an asynchronous refresh and returns, leaving the caller to use the
 * current (possibly slightly stale) snapshot.
 */
static void
sysfs_snap_ensure(void)
{
    if (g_lock == nullptr) {
        return;
    }

    SInt32 gen = IORegistryEntry::getGenerationCount();

    IOLockLock(g_lock);
    if (g_snapshot != nullptr) {
        uint64_t now, elapsed_ns;
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - g_snapshot->uptime, &elapsed_ns);

        if ((g_snap_gen_valid && g_snap_gen == gen) ||
            elapsed_ns <= SYSFS_SNAP_TTL_NS) {
            IOLockUnlock(g_lock);
            return;             /* registry unchanged, or still within the TTL */
        }
    }

    /*
     * Stale (or absent). Ask the worker to refresh - but not if we rebuilt
     * recently, unless there is no snapshot at all to serve from. Serving
     * slightly out-of-date device topology costs nothing; re-walking the
     * registry every time a driver attaches costs the whole system.
     */
    if (g_snapshot != nullptr && g_last_build_valid) {
        uint64_t now, since_ns;
        clock_get_uptime(&now);
        absolutetime_to_nanoseconds(now - g_last_build_uptime, &since_ns);
        if (since_ns < SYSFS_SNAP_MIN_REBUILD_NS) {
            IOLockUnlock(g_lock);
            return;
        }
    }

    if (!g_refresh_pending && !g_building && g_refresh_call != nullptr) {
        g_refresh_pending = true;
        g_building = true;
        IOLockUnlock(g_lock);
        thread_call_enter(g_refresh_call);
        return;
    }
    IOLockUnlock(g_lock);
}

#pragma mark - display names

/* Compose the unique display name of the index-th child of node `pidx` into buf
 * (base IOKit name, plus an "@<n>" suffix if earlier siblings share the base
 * name), and its regid into *child_regid. Returns true if that child exists.
 * Caller holds g_lock. */
static bool
sysfs_child_display(const struct sysfs_snapshot *snap, uint32_t pidx, uint32_t index, char *buf, size_t buflen,
                    uint64_t *child_regid)
{
    if (snap == nullptr || pidx >= snap->node_count || index >= snap->nodes[pidx].num_children) {
        return false;
    }
    uint32_t base = snap->nodes[pidx].first_child;
    uint32_t ci = snap->child_idx[base + index];
    const char *nm = snap->nodes[ci].name;

    uint32_t dup = snap->nodes[ci].dup_ordinal;
    if (dup == 0) {
        strlcpy(buf, nm, buflen);
    } else {
        snprintf(buf, buflen, "%s@%u", nm, dup);
    }
    *child_regid = snap->nodes[ci].regid;
    return true;
}

#pragma mark - C API

extern "C" void
sysfs_iokit_init(void)
{
    if (g_lock == nullptr) {
        g_lock = IOLockAlloc();
    }
    if (g_refresh_call == nullptr) {
        g_refresh_call = thread_call_allocate(sysfs_snap_refresh_cb, nullptr);
    }
}

/*
 * Build the first snapshot synchronously. Called from the mount path, which is
 * a safe context to walk the registry from (no VFS locks held, unlike a vnop),
 * so /sys is usable immediately instead of returning empty until the first
 * asynchronous refresh lands.
 */
extern "C" void
sysfs_iokit_prime(void)
{
    if (g_lock == nullptr) {
        return;
    }
    IOLockLock(g_lock);
    bool have = (g_snapshot != nullptr) || g_building;
    if (!have) {
        g_building = true;
    }
    IOLockUnlock(g_lock);

    if (!have) {
        sysfs_snap_refresh();
    }
}

extern "C" void
sysfs_iokit_teardown(void)
{
    /*
     * Stop the refresh worker BEFORE tearing anything down, and wait for an
     * in-flight run to finish - it touches g_snapshot and g_lock.
     *
     * thread_call_cancel_wait() would do both in one call but is NOT in the
     * third-party kext KPI export set (the kext fails to bind and will not
     * load), so cancel any queued run and then wait for a running one on our own
     * lock. Sleeping here is safe: this is the kext stop path, with no VFS locks
     * held - unlike a vnop, which must never block (see the header comment).
     */
    if (g_refresh_call != nullptr) {
        thread_call_cancel(g_refresh_call);
        if (g_lock != nullptr) {
            IOLockLock(g_lock);
            while (g_building) {
                IOLockSleep(g_lock, &g_building, THREAD_UNINT);
            }
            IOLockUnlock(g_lock);
        }
        thread_call_free(g_refresh_call);
        g_refresh_call = nullptr;
    }

    if (g_lock != nullptr) {
        IOLockLock(g_lock);
        struct sysfs_snapshot *old_snap = g_snapshot;
        g_snapshot = nullptr;
        g_snap_gen_valid = false;
        g_building = false;
        /*
         * Reset the rate-limit and sizing state too, so a reload starts clean
         * rather than inheriting a stale build timestamp (which would suppress
         * the first refresh) or a node-count estimate from the previous load.
         */
        g_last_build_valid = false;
        g_last_build_uptime = 0;
        g_last_node_count = 0;
        g_refresh_pending = false;
        IOLockUnlock(g_lock);
        sysfs_snap_free(old_snap);
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
    sysfs_snap_ensure();
    IOLockLock(g_lock);
    int found = (sysfs_hash_find(g_snapshot, regid) != SYSFS_IDX_NONE) ? 1 : 0;
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
    sysfs_snap_ensure();
    IOLockLock(g_lock);
    uint32_t pidx = sysfs_hash_find(g_snapshot, regid);
    int ok = 0;
    if (pidx != SYSFS_IDX_NONE) {
        ok = sysfs_child_display(g_snapshot, pidx, (uint32_t)index, namebuf, buflen, child_regid) ? 1 : 0;
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
    sysfs_snap_ensure();
    IOLockLock(g_lock);
    int found = 0;
    uint32_t pidx = sysfs_hash_find(g_snapshot, regid);
    if (pidx != SYSFS_IDX_NONE) {
        char buf[SYSFS_IOKIT_NAMEMAX + 16];
        uint32_t n = g_snapshot->nodes[pidx].num_children;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t id = 0;
            if (!sysfs_child_display(g_snapshot, pidx, i, buf, sizeof(buf), &id)) {
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
    sysfs_snap_ensure();
    IOLockLock(g_lock);
    int ok = 0;
    uint32_t idx = sysfs_hash_find(g_snapshot, regid);
    if (idx != SYSFS_IDX_NONE) {
        *parent_regid = g_snapshot->nodes[idx].parent_regid;
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
    sysfs_snap_ensure();
    IOLockLock(g_lock);
    size_t n = 0;
    uint32_t idx = sysfs_hash_find(g_snapshot, regid);
    if (idx != SYSFS_IDX_NONE) {
        strlcpy(buf, g_snapshot->nodes[idx].name, buflen);
        n = strlen(buf);
    }
    IOLockUnlock(g_lock);
    return n;
}
