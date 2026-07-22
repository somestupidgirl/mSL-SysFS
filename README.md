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

> **Status: scaffold.** This repository currently contains the project skeleton
> and the core filesystem infrastructure only. `/sys` **mounts as an essentially
> empty filesystem** — the root plus the fixed Linux `/sys` top-level directories
> — and the build/load/mount flow works. All *content* (device attributes, the
> symlink views, the `sysfsd` daemon, IOKit enumeration) is not implemented yet.
> See [Feature status](#feature-status).

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

As in the procfs sibling, IOKit is C++/registry territory that a kext reaches
awkwardly and that PAC/SIP restrict, so the plan is a **`sysfsd` userspace
daemon** plus a C++ IOKit translation unit (`sysfs_iokit.cpp`) — mirroring
`procfsd` / `procfs_iokit.cpp` — feeding the kext the registry snapshot it
formats into nodes. Anything unreachable degrades gracefully (empty directory /
`ENOTSUP`) rather than failing the mount.

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
  - `stat(2)` on every skeleton node (world-readable, root-owned: directories
    `0555`, files `0444`).
  - Clean unmount and kext unload (no leaked vnodes).

**Planned (not yet implemented):**

  - The `sysfsd` daemon and its kernel-control wire protocol.
  - The IOKit translation unit and the `/sys/devices` registry mirror.
  - The `class/`, `bus/`, `block/`, `dev/` symlink views.
  - `module/`, `kernel/`, `firmware/`, `fs/`, `power/` content.
  - GUI / preference pane, installer package, and the test suite.

## Repository layout

```
include/fs/sysfs/sysfs.h   shared node model (kernel + future daemon)
kext/                      the kernel extension
  sysfs.c                    kmod start/stop, init/fini
  sysfs_vfsops.c             VFS ops: mount/unmount/root/getattr
  sysfs_vnops.c              vnode ops: lookup/readdir/getattr/read/reclaim
  sysfs_node.c               sfsnode hash table + find/create
  sysfs_structure.c          the static /sys skeleton tree
  sysfs_subr.c               generic helpers (allocvp, fileid, sizes)
fs/                        the mount bundle (sysfs.fs) + mount_sysfs
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
sudo umount /tmp/sys
sudo make -C kext unload
```

## Credits

Built to the same standards as, and structurally derived from, the mSL/ProcFS
kernel extension (itself descended from Kim Topley's macOS procfs). See
[mSL/ProcFS](https://github.com/somestupidgirl/mSL-ProcFS).

## License

MIT — see [LICENSE](LICENSE).
