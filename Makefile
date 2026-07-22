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
else ifeq ($(ARCH),universal)
    # The universal kextfs target builds each arch explicitly and lipos them;
    # these defaults just need to be valid (the arm64e slice).
    KEXT_ARCHFLAGS    := -arch arm64e
    KEXT_TRIPLE       := arm64e-apple-macos12.0
    FS_ARCHFLAGS      := -arch arm64
    FS_TRIPLE         := arm64-apple-macos12.0
    LIB_ARCHFLAGS     := -arch arm64e
    LIB_TRIPLE        := arm64e-apple-macos12.0
else
    $(error Unknown ARCH=$(ARCH). Use arm64e, x86_64, or universal)
endif

KEXT_FLAGS := ARCHFLAGS="$(KEXT_ARCHFLAGS)" TARGET_TRIPLE="$(KEXT_TRIPLE)"
FS_FLAGS   := ARCHFLAGS="$(FS_ARCHFLAGS)"   TARGET_TRIPLE="$(FS_TRIPLE)"
LIB_FLAGS  := ARCHFLAGS="$(LIB_ARCHFLAGS)"  TARGET_TRIPLE="$(LIB_TRIPLE)"

# The build wipes and repopulates $(OUT) in a fixed order; never parallelise it.
.NOTPARALLEL:

# ---------------------------------------------------------------------------
# Build  ->  $(OUT)
# ---------------------------------------------------------------------------

# `clean` first so a build always starts from a clean tree. This matters when
# several arches are built in one checkout (e.g. CI runs `make ARCH=arm64e` then
# `make ARCH=x86_64`): the single-arch kextfs only wipes $(OUT), so without this
# the second build would relink the first arch's stale .o/.a files and fail with
# "found architecture 'arm64e.kernel', required architecture 'x86_64'".
all: clean kextfs

ifeq ($(ARCH),universal)

# kext + fs as fat (arm64e + x86_64) binaries: build each arch, then lipo the
# Mach-O executables together and re-sign the bundles.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C kext ARCHFLAGS="-arch arm64e" TARGET_TRIPLE="arm64e-apple-macos12.0"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch arm64"  TARGET_TRIPLE="arm64-apple-macos12.0"
	mv kext/sysfs.kext kext/sysfs.kext.dSYM fs/sysfs.fs fs/sysfs.fs.dSYM $(OUT)
	mv $(OUT)/sysfs.kext $(OUT)/sysfs.kext.arm64e
	mv $(OUT)/sysfs.fs   $(OUT)/sysfs.fs.arm64
	$(MAKE) -C kext clean
	$(MAKE) -C fs clean
	$(MAKE) -C lib clean
	$(MAKE) -C lib  ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C kext ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	$(MAKE) debug -C fs   ARCHFLAGS="-arch x86_64" TARGET_TRIPLE="x86_64-apple-macos10.15"
	rm -rf $(OUT)/sysfs.kext.dSYM $(OUT)/sysfs.fs.dSYM
	mv kext/sysfs.kext kext/sysfs.kext.dSYM fs/sysfs.fs fs/sysfs.fs.dSYM $(OUT)
	mv $(OUT)/sysfs.kext $(OUT)/sysfs.kext.x86_64
	mv $(OUT)/sysfs.fs   $(OUT)/sysfs.fs.x86_64
	cp -r $(OUT)/sysfs.kext.arm64e $(OUT)/sysfs.kext
	lipo -create $(OUT)/sysfs.kext.arm64e/Contents/MacOS/sysfs $(OUT)/sysfs.kext.x86_64/Contents/MacOS/sysfs -output $(OUT)/sysfs.kext/Contents/MacOS/sysfs
	cp -r $(OUT)/sysfs.fs.arm64 $(OUT)/sysfs.fs
	lipo -create $(OUT)/sysfs.fs.arm64/Contents/Resources/mount_sysfs $(OUT)/sysfs.fs.x86_64/Contents/Resources/mount_sysfs -output $(OUT)/sysfs.fs/Contents/Resources/mount_sysfs
	codesign --force --timestamp=none --sign - $(OUT)/sysfs.kext
	codesign --force --timestamp=none --sign - $(OUT)/sysfs.fs
	rm -rf $(OUT)/sysfs.kext.arm64e $(OUT)/sysfs.kext.x86_64
	rm -rf $(OUT)/sysfs.fs.arm64 $(OUT)/sysfs.fs.x86_64

else

# kext + fs for a single arch.
kextfs:
	rm -rf $(OUT)
	mkdir $(OUT)
	$(MAKE) -C lib  $(LIB_FLAGS)
	$(MAKE) debug -C kext $(KEXT_FLAGS)
	$(MAKE) debug -C fs   $(FS_FLAGS)
	mv kext/sysfs.kext kext/sysfs.kext.dSYM fs/sysfs.fs fs/sysfs.fs.dSYM $(OUT)

endif

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
