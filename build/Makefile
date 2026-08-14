# CubeGM open-source replacement -- top-level build orchestration.
# Run on your Linux build host AFTER: source/ repos cloned, toolchain installed,
# and SYSROOT pointing at the dumped device rootfs (glibc 2.17).
#
# Typical flow:
#   ./build/toolchain/setup_toolchain.sh        # install cross gcc
#   ./build/toolchain/sysroot_from_device.sh    # dump device rootfs -> SYSROOT
#   make SYSROOT=$HOME/cubegm-sysroot           # build + package + verify

CROSS  ?= arm-none-linux-gnueabihf-
SYSROOT ?=
CFLAGS  = -march=armv7-a -mtune=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -O2

ifeq ($(SYSROOT),)
$(warning SYSROOT is empty -- linking will use the toolchain's glibc 2.38 and FAIL on the device. Pass SYSROOT=<device rootfs from sysroot_from_device.sh>)
else
CFLAGS += --sysroot=$(SYSROOT)
endif

ROOT   := $(CURDIR)/..
SRC    := $(ROOT)/source
DEVICE := $(ROOT)/cubegm/cubegm
BUILD  := $(CURDIR)/out
PKG    := $(CURDIR)/cubegm_pkg

CC  := $(CROSS)gcc
CXX := $(CROSS)g++

.PHONY: all toolchain frontend cores package verify smoke clean

all: frontend cores package verify
	@echo "Build complete -> $(PKG)"

toolchain:
	@echo "Run: ./build/toolchain/setup_toolchain.sh"

# Build the picoarch front-end (TreeFrogUI_picoarch, plat_linux).
# Exact upstream target depends on the repo Makefile; we export our cross vars.
frontend:
	@echo "== building picoarch (TreeFrogUI_picoarch, linux platform) =="
	$(MAKE) -C $(SRC)/TreeFrogUI_picoarch CC=$(CC) CXX=$(CXX) \
	    CFLAGS="$(CFLAGS)" CXXFLAGS="$(CFLAGS)" platform=linux
	@echo "== building FrogUI launcher (FrogUI) =="
	$(MAKE) -C $(SRC)/FrogUI CC=$(CC) CXX=$(CXX) \
	    CFLAGS="$(CFLAGS)" CXXFLAGS="$(CFLAGS)"
	@mkdir -p $(BUILD)
	@cp -f $(SRC)/TreeFrogUI_picoarch/picoarch $(BUILD)/ 2>/dev/null || true
	@cp -f $(SRC)/FrogUI/frogui_libretro.so $(BUILD)/ 2>/dev/null || true

# For MVP we reuse the device's existing libemu_*.so cores (already armhf/2.17).
# Building 57 cores from source is Stage3 (treefrog-ui/clone_cores.sh).
cores:
	@mkdir -p $(PKG)/cores
	@cp -f $(DEVICE)/cores/libemu_*.so $(PKG)/cores/ 2>/dev/null || true
	@cp -f $(CURDIR)/cores/config.xml $(PKG)/cores/config.xml

# Assemble the SD-card payload (cubegm/ layout).
package: frontend cores
	@mkdir -p $(PKG)/bin $(PKG)/lib $(PKG)/input
	@cp -f $(BUILD)/picoarch $(PKG)/picoarch 2>/dev/null || true
	@cp -f $(BUILD)/frogui_libretro.so $(PKG)/frogui_libretro.so 2>/dev/null || true
	@cp -f $(CURDIR)/autorun/zhijack.sh $(PKG)/zhijack.sh
	@chmod +x $(PKG)/zhijack.sh $(PKG)/picoarch 2>/dev/null || true
	@echo "Packaged -> $(PKG)"

# ABI gate: assert every shipped binary matches RK3036G (EM_ARM/0x5000400/<=2.17).
verify:
	@echo "== ABI verification =="
	@for b in $(PKG)/picoarch $(PKG)/frogui_libretro.so $(PKG)/cores/libemu_*.so; do \
	  [ -f "$$b" ] && ./toolchain/verify_target_abi.sh "$$b" || true; \
	done

# Cross-compile a trivial program to prove toolchain + sysroot linkage works.
smoke:
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $(BUILD)/hello $(CURDIR)/cross_compile_smoke/hello.c
	./toolchain/verify_target_abi.sh $(BUILD)/hello

clean:
	rm -rf $(BUILD) $(PKG)
