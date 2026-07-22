#
# All-in-one Makefile - mSL/SysFS (scaffold)
#
# Usage:
#   make                    # build libs + kext + fs into $(OUT)
#   make ARCH=arm64e        # Apple Silicon kext + arm64 fs (default on arm64)
#   make ARCH=x86_64        # Intel kext + x86_64 fs
#   sudo make install       # install the built artifacts (run AFTER make)
#   sudo make uninstall     # remove them
#   make clean              # remove build artifacts (no sudo needed)
#
# The GUI, installer package (.pkg/.dmg), the sysfsd daemon and the test suite
# are later passes (see README "Feature status"); this scaffold Makefile builds
# only the kext, the sysfs.fs mount bundle and their supporting libraries.
#
# NOTE: never run the build as root. `make install` only COPIES the already-built
# artifacts from $(OUT) into place; it does not compile. This keeps every build
# artifact owned by the invoking user, so `make clean` never needs sudo.
#

MAKE=make
OUT=out

# Install locations.
EXT_DIR        := /Library/Extensions
FS_DIR         := /Library/Filesystems

# Detect native arch if ARCH not specified
NATIVE_ARCH := $(shell uname -m)
ifeq ($(NATIVE_ARCH),arm64)
    DEFAULT_ARCH := arm64e
else
    DEFAULT_ARCH := x86_64
endif
ARCH ?= $(DEFAULT_ARCH)
# Accept arm64 as alias for arm64e (kexts require arm64e ABI)
ifeq ($(ARCH),arm64)
    override ARCH := arm64e
endif

# Per-arch settings
ifeq ($(ARCH),arm64e)
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else ifeq ($(ARCH),x86_64)
    KEXT_ARCHFLAGS    := -arch x86_64
    KEXT_TRIPLE       := x86_64-apple-macos10.15
    FS_ARCHFLAGS      := -arch x86_64
    FS_TRIPLE         := x86_64-apple-macos10.15
    LIB_ARCHFLAGS     := -arch x86_64
    LIB_TRIPLE        := x86_64-apple-macos10.15
else
    $(error Unknown ARCH=$(ARCH). Use arm64e or x86_64)
endif

KEXT_FLAGS := ARCHFLAGS="$(KEXT_ARCHFLAGS)" TARGET_TRIPLE="$(KEXT_TRIPLE)"
FS_FLAGS   := ARCHFLAGS="$(FS_ARCHFLAGS)"   TARGET_TRIPLE="$(FS_TRIPLE)"
LIB_FLAGS  := ARCHFLAGS="$(LIB_ARCHFLAGS)"  TARGET_TRIPLE="$(LIB_TRIPLE)"

# The build wipes and repopulates $(OUT) in a fixed order; never parallelise it.
.NOTPARALLEL:

# ---------------------------------------------------------------------------
# Build  ->  $(OUT)
# ---------------------------------------------------------------------------

all: kextfs

kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  $(LIB_FLAGS)
	$(MAKE) debug -C kext $(KEXT_FLAGS)
	$(MAKE) debug -C fs   $(FS_FLAGS)
	mv kext/sysfs.kext kext/sysfs.kext.dSYM fs/sysfs.fs fs/sysfs.fs.dSYM $(OUT)

# ---------------------------------------------------------------------------
# Install / uninstall  (operate on the already-built $(OUT); need root)
# ---------------------------------------------------------------------------

install:
	test -d "$(OUT)/sysfs.kext"
	sudo cp -r "$(OUT)/sysfs.kext" "$(EXT_DIR)/sysfs.kext"
	sudo chmod -R 755 "$(EXT_DIR)/sysfs.kext"
	sudo chown -R root:wheel "$(EXT_DIR)/sysfs.kext"
	sudo cp -r "$(OUT)/sysfs.fs" "$(FS_DIR)/sysfs.fs"
	sudo chmod -R 755 "$(FS_DIR)/sysfs.fs"
	sudo chown -R root:wheel "$(FS_DIR)/sysfs.fs"

uninstall:
	sudo rm -rf "$(EXT_DIR)/sysfs.kext" || true
	sudo rm -rf "$(FS_DIR)/sysfs.fs" || true

clean:
	$(MAKE) -C lib clean || true
	$(MAKE) -C kext clean || true
	$(MAKE) -C fs clean || true
	rm -rf $(OUT)

.PHONY: all kextfs install uninstall clean
