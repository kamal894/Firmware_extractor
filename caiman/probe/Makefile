# SPDX-License-Identifier: MIT
#
# make                      -> out/caiman/probe.Image + out/caiman/probe-boot.img
# make PLATFORM=qemu        -> out/qemu/probe.Image (for test/qemu_test.sh)
#
# Options (all optional):
#   PROBE_UART=0            never touch the debug UART
#   PROBE_PAINT=1           paint colour bars into the frame buffer it finds
#   PROBE_FINISH=0|1|2      0 warm reboot (default), 1 cold reboot, 2 hang
#   PROBE_FINISH_DELAY_MS=N wait before rebooting (default 3000)
#   PROBE_CRASHTEST=1       fault on purpose at the end (tests the handler)
#   BOOTIMG_ARGS="..."      extra args for tools/pack_bootimg.py, e.g.
#                           "--stock-boot ../../out/boot.img" or "--compress lz4"

PLATFORM ?= caiman
OUT ?= out/$(PLATFORM)

CC := clang
LD := ld.lld
OBJCOPY := llvm-objcopy
READELF := llvm-readelf
PYTHON ?= python3

PROBE_UART ?= 1
PROBE_PAINT ?= 0
PROBE_FINISH ?= 0
PROBE_FINISH_DELAY_MS ?= 3000
PROBE_CRASHTEST ?= 0
VERSION := $(shell git describe --always --dirty 2>/dev/null || echo dev)

CFLAGS := --target=aarch64-none-elf -march=armv8.2-a -O2 -g -std=gnu11 \
	-ffreestanding -fno-builtin -nostdlib -fno-stack-protector -fno-common \
	-mgeneral-regs-only -mstrict-align -fPIE -fvisibility=hidden \
	-fno-jump-tables -fno-unwind-tables -fno-asynchronous-unwind-tables \
	-Wall -Wextra -Wno-unused-parameter \
	-DPROBE_VERSION='"$(VERSION)"' \
	-DPROBE_UART=$(PROBE_UART) -DPROBE_PAINT=$(PROBE_PAINT) \
	-DPROBE_FINISH=$(PROBE_FINISH) -DPROBE_FINISH_DELAY_MS=$(PROBE_FINISH_DELAY_MS) \
	-DPROBE_CRASHTEST=$(PROBE_CRASHTEST)
ifeq ($(PLATFORM),qemu)
CFLAGS += -DPLATFORM_QEMU
endif
LDFLAGS := -pie --no-dynamic-linker -z norelro -z noexecstack --build-id=none -T linker.ld

SRCS := start.S main.c log.c uart.c timer.c fdt.c psci.c exception.c display.c lib.c
OBJS := $(patsubst %,$(OUT)/%.o,$(SRCS))

all: $(OUT)/probe.Image $(if $(filter caiman,$(PLATFORM)),$(OUT)/probe-boot.img)

$(OUT)/%.o: % probe.h platform.h platform_caiman.h platform_qemu.h Makefile
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUT)/probe.elf: $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)
	@if $(READELF) -r $@ | grep -q R_AARCH64; then \
		echo "error: $@ has relocations; the image must be position independent"; \
		$(READELF) -r $@; rm -f $@; exit 1; fi

$(OUT)/probe.Image: $(OUT)/probe.elf
	$(OBJCOPY) -O binary $< $@
	@$(PYTHON) ../tools/check_image.py $@

$(OUT)/probe-boot.img: $(OUT)/probe.Image ../tools/pack_bootimg.py
	$(PYTHON) ../tools/pack_bootimg.py pack --kernel $< --out $@ $(BOOTIMG_ARGS)

clean:
	rm -rf out

.PHONY: all clean
