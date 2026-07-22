#
# All-in-one Makefile
#
# libcpuft and libreadfile are intentionally not part of the scaffold: nothing
# in sysfs uses per-CPU feature detection or read_file() yet. Add a submodule
# back here (and link it with its -I include path in kext/Makefile) when a
# consumer appears.
#

MAKE=make

all: debug

debug:
	$(MAKE) -C libbsdmalloc
	$(MAKE) -C libsbuf
	$(MAKE) -C libkext

release: TARGET=release
release: debug

clean:
	$(MAKE) -C libbsdmalloc clean
	$(MAKE) -C libsbuf clean
	$(MAKE) -C libkext clean

.PHONY: all debug release clean
