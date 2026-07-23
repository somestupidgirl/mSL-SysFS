# mSL/SysFS

[![C/C++ CI](https://github.com/somestupidgirl/mSL-SysFS/actions/workflows/c-cpp.yml/badge.svg)](https://github.com/somestupidgirl/mSL-SysFS/actions/workflows/c-cpp.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Platform](https://img.shields.io/badge/Platform-macOS-lightgrey)](#how-to-build-sysfs)

mSL/SysFS is a kernel-extension implementation of the Linux `/sys` (sysfs) file
system for macOS, exposing the system's device model — buses, classes, devices
and their attributes — as a filesystem.

**macOS Subsystem for Linux / SysFS** — a native kernel-extension implementation
of `/sys` for macOS, presenting macOS's own device registry through the
Linux-compatible sysfs layout.

One module of **mSL/XNU**, a modular macOS Subsystem for Linux.

> **Status: early.** `/sys` mounts, and **`/sys/devices` now mirrors the IOKit
> registry** — every registry entry appears as a directory (recursively, keyed by
> `IORegistryEntryID`) with a readable `name` attribute, walked in-kernel with no
> daemon. The other top-level directories (`class/`, `bus/`, `block/`, `dev/`,
> `module/`, `kernel/`, …) are still the empty skeleton, and per-device attributes
> beyond `name` are not enumerated yet. See [Feature status](#feature-status).

## The larger project

mSL/XNU — *macOS Subsystem for Linux / X is Now UNIX* — aims at **native,
seamless execution of Linux ELF binaries on macOS**: not in a container and not
in a virtual machine, but as ordinary processes on the running system.

Reaching that needs several independent pieces, which is why the project is
modular rather than one monolith. Each is useful on its own, and each can be
installed, replaced or omitted:

| Piece | What it does | Where |
|-------|--------------|-------|
| **Filesystem Hierarchy Standard** | The Linux filesystem layout, natively | [mSL/FHS](https://github.com/somestupidgirl/mSL-FHS) |
| **Syscall translation** | Linux system calls onto Darwin's, over `Hypervisor.framework` | [mSL/NABI](https://github.com/somestupidgirl/mSL-NABI) |
| **procfs** | `/proc`, as a real filesystem | [mSL/ProcFS](https://github.com/somestupidgirl/mSL-ProcFS) |
| **sysfs** | `/sys`, likewise | **this repository** |
| **devfs** | `/dev` — already part of macOS | XNU |

**This repository is the SysFS piece**, and it is a work in progress. The
rest of this document describes it.

## What is sysfs?

On Linux, *sysfs* is a virtual filesystem (mounted at `/sys`) that exports the
kernel's *device model* to userspace: the buses on the system, the device
classes, every device object, the drivers bound to them, and each object's
tunable/informational *attributes* — all as directories and small text files.
Almost everything in `/sys` is a view of one underlying tree of *kobjects*; the
class/, bus/, block/ and dev/ hierarchies are largely symbolic links into the
canonical `/sys/devices` tree.

macOS has no `/sys`, but it has the perfect analog for the device model itself:
the **IOKit registry** (the IORegistry). The IORegistry is a live tree of device
objects (`IOService` nodes) carrying typed properties, addressable by a stable
64-bit `IORegistryEntryID` — structurally the same idea as Linux's kobject tree.
SysFS is built on that mapping.

### Design: `/sys/devices` mirrors the IORegistry

The core of SysFS (in the passes that follow this scaffold) is:

| sysfs path | macOS source |
|------------|--------------|
| `devices/` | the IORegistry itself — each registry entry becomes a directory, each IOKit property becomes an attribute file, keyed by `IORegistryEntryID` |
| `class/`   | device-class groupings (`net`, `tty`, `power_supply`, `thermal`, …), symlinks into `devices/` |
| `bus/`     | per-bus `devices/` + `drivers/`, from IOKit provider families (PCI, USB, …) |
| `block/`   | block devices (`IOMedia`), symlinks into `devices/` |
| `dev/`     | `char/` and `block/` `major:minor` symlinks into `devices/` |
| `module/`  | loaded kernel modules — macOS kexts (`kextstat`-equivalent) |
| `kernel/`  | kernel tunables and info, from the sysctl MIB |
| `firmware/`| the device tree (`firmware/devicetree`), plus ACPI/DMI where present |
| `fs/`      | filesystem-specific tunables |
| `power/`   | system power-management state |
| `hypervisor/` | hypervisor interface (empty when not running under one) |

IOKit registry traversal, entry ids and property reads are all *public in-kernel
KPI* (`com.apple.kpi.iokit`), so — unlike the procfs sibling, whose daemon exists
for `task_for_pid`/VM introspection that genuinely can't be done in-kernel —
SysFS walks the registry **directly in the kext**, with no `sysfsd` daemon. This
lives in one C++ translation unit (`kext/sysfs_iokit.cpp`) exposing a small
`extern "C"` surface to the C filesystem code; it builds against the **plain
macOS SDK only** (no MacKernelSDK — the C++ runtime symbols resolve at load
against `com.apple.kpi.libkern`). Anything unreachable degrades gracefully
(empty directory) rather than failing the mount.

The node model already reflects this: a sysfs node's identity
(`struct sfsid`, `include/fs/sysfs/sysfs.h`) is keyed on the backing
`IORegistryEntryID`, so the dynamic device tree drops in without reworking the
core.

## Feature status

**Working:**

  - The kext loads and registers the `sysfs` VFS type.
  - `mount_sysfs` mounts `/sys` as a local, read-only filesystem.
  - Directory listing (`ls`, `find`, `readdir(3)`, `getdirentries64(2)`) of the
    root, returning the fixed Linux `/sys` top-level directories:
    `block bus class dev devices firmware fs hypervisor kernel module power`
    (and `dev/char`, `dev/block`).
  - `stat(2)` on every node (world-readable, root-owned: directories `0555`,
    files `0444`).
  - **`/sys/devices` mirrors the IOKit registry**: an in-kernel C++ IOKit
    translation unit walks `gIOServicePlane`, so each registry entry appears as a
    directory (recursively, keyed by `IORegistryEntryID`, named by its IOKit
    name with sibling-collision suffixes) with a readable `name` attribute file.
  - Clean unmount and kext unload (no leaked vnodes).

**Planned (not yet implemented):**

  - Full IOKit property → attribute enumeration (beyond `name`).
  - The `class/`, `bus/`, `block/`, `dev/` symlink views into `devices/`.
  - `module/`, `kernel/`, `firmware/`, `fs/`, `power/` content.
  - GUI / preference pane, installer package, and the test suite.

## Repository layout

```
include/fs/sysfs/sysfs.h       shared node model
include/fs/sysfs/sysfs_iokit.h C surface of the IOKit translation unit
kext/                      the kernel extension
  sysfs.c                    kmod start/stop, init/fini
  sysfs_vfsops.c             VFS ops: mount/unmount/root/getattr
  sysfs_vnops.c              vnode ops: lookup/readdir/getattr/read/reclaim
  sysfs_node.c               sfsnode hash table + find/create
  sysfs_structure.c          the /sys skeleton tree (+ the devices node)
  sysfs_subr.c               generic helpers (allocvp, fileid, sizes)
  sysfs_iokit.cpp            in-kernel IORegistry walk (the one C++ TU)
fs/                        the mount bundle (sysfs.fs) + mount_sysfs
tools/                     boot auto-mount: mount-sysfs + com.beako.sysfs.plist
lib/                       vendored libraries (git submodules)
include/xnu/               vendored XNU private headers
```

## How to build sysfs

Prerequisites: Xcode command-line tools, and the submodules + vendored headers
checked out:

```bash
git submodule update --init --recursive
```

Then build the libraries, kext and mount bundle into `out/`:

```bash
make                    # native arch (arm64e on Apple Silicon)
make ARCH=x86_64        # Intel
```

### Loading and mounting (Apple Silicon, macOS)

Loading a third-party kext requires the usual reduced-security posture
(Recovery → Startup Security Utility → *Reduced Security* + *Allow user
management of kernel extensions*), then approval in System Settings on first
load. These steps need administrator rights.

```bash
sudo make -C kext load          # load the kext
kextstat | grep sysfs           # confirm it registered
mkdir -p /tmp/sys
sudo ./out/sysfs.fs/Contents/Resources/mount_sysfs sysfs /tmp/sys
ls /tmp/sys                     # block bus class dev devices firmware ...
ls /tmp/sys/devices             # the IOKit registry mirror
sudo umount /tmp/sys
sudo make -C kext unload
```

### Auto-mounting at boot

`sudo make install` copies the kext and `sysfs.fs` into place and installs the
auto-mount **LaunchDaemon** (`com.beako.sysfs`): a system daemon that, at boot,
runs `/usr/local/sbin/mount-sysfs` — which loads the kext if needed and mounts
sysfs at `/sys`. Because `/sys` lives on the read-only system volume, the install
also adds `sys` to `/etc/synthetic.conf` so the mount point is created at boot.

Mounting `/sys` needs root, so this is a system LaunchDaemon, not a per-user
login agent (mirroring how the procfs sibling mounts `/proc`). To avoid a fault
in the kernel code boot-looping the machine, auto-mount stays **disarmed** until
you create the arm flag — exactly like procfs's `/var/db/procfs.enabled`:

```bash
sudo make install               # installs kext, fs, and the auto-mount daemon
sudo touch /var/db/sysfs.enabled # arm auto-mount (one time)
sudo reboot                      # /sys is created, kext loads, sysfs mounts
```

`rm /var/db/sysfs.enabled` disables auto-mount again; `sudo make uninstall`
removes the daemon, the `sys` synthetic entry, and unmounts `/sys`.

## Credits

Built to the same standards as, and structurally derived from, the mSL/ProcFS
kernel extension (itself descended from Kim Topley's macOS procfs). See
[mSL/ProcFS](https://github.com/somestupidgirl/mSL-ProcFS).

## License

MIT — see [LICENSE](LICENSE).
