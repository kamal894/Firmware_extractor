# SPDX-License-Identifier: MIT
#
# make                      -> out/caiman/probe.Image + out/caiman/probe-boot.img
# make PLATFORM=qemu        -> out/qemu/probe.Image (for test/qemu_test.sh)
#
# Options (all optional):
#   PROBE_UART=0            never touch the debug UART
#   PROBE_PAINT=1           paint colour bars into the frame buffer it finds, send the
#                           frame (command-mode panel) and keep the watchdog fed
#   PROBE_FINISH=0|1|2      0 warm reboot (default), 1 cold reboot, 2 hang
#   PROBE_FINISH_DELAY_MS=N wait before rebooting (default 3000)
#   PROBE_CRASHTEST=1       fault on purpose at the end (tests the handler)
#   PROBE_TOUCH=1           read-only look at the touch screen's power (PMIC through ACPM), pins, SPI
#   PROBE_TOUCH=9           with LOADER=1: the touch chip powered, its SPI set up and the chip identified for XNU (touchscreen=2)
#   PROBE_SHOTS=N           50 pictures of the panel, one a second from XNU's N-th, into pstore's pmsg zone (watcher.c)
#   PROBE_DVFS=1|2          CPU cluster clocks through ACPM: 1 read them, 2 also raise them (dvfs.c)
#   PROBE_TO_FASTBOOT=1     the watcher's reset at the end lands in fastboot (reboot mode 0xfc)
#   PROBE_WIFI=1            the Wi-Fi's PCIe (RC1) registers and pins as the bootloader left them, read only (wifi.c)
#   PROBE_PANEL=1           the panel over DSIM0: power mode and brightness read, brightness set and put back (panel.c)
#   PROBE_BATTERY=1         the battery read from the MAX77779 (hsi2c_6) and shared with XNU (battery.c)
#   PROBE_G3D_KHZ=N         with PROBE_DVFS=2: the GPU's clock (default 580000; the DT's steps go to 940000)
#   PROBE_GPU=1             the Mali GPU: power domains and its first registers, read only (gpu.c)
#   LOADER=1                the M2 loader: probe, then start XNU from an appended payload
#                           (OUT=out/loader-$(PLATFORM); ../tools/mkxnuimg.py builds the image)
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
ifdef PROBE_UFS
CFLAGS += -DPROBE_UFS=$(PROBE_UFS)
endif
ifdef PROBE_GPU
CFLAGS += -DPROBE_GPU=$(PROBE_GPU)
endif
# MALI_FW=path/to/mali_csffw-r54p3.bin: the GPU firmware built into the image (gpu_fw.S), for LOADER=1
ifdef MALI_FW
CFLAGS += -DMALI_FW_EMBEDDED=1 -DMALI_FW_PATH='"$(MALI_FW)"'
SRCS_FW := gpu_fw.S
endif
ifdef PROBE_SHOTS
CFLAGS += -DPROBE_SHOTS=$(PROBE_SHOTS)
endif
ifdef PROBE_DVFS
CFLAGS += -DPROBE_DVFS=$(PROBE_DVFS)
endif
ifdef PROBE_WIFI
CFLAGS += -DPROBE_WIFI=$(PROBE_WIFI)
endif
ifdef PROBE_PANEL
CFLAGS += -DPROBE_PANEL=$(PROBE_PANEL)
endif
ifdef PROBE_BATTERY
CFLAGS += -DPROBE_BATTERY=$(PROBE_BATTERY)
endif
ifdef PROBE_G3D_KHZ
CFLAGS += -DPROBE_G3D_KHZ=$(PROBE_G3D_KHZ)
endif
ifdef PROBE_TOUCH
CFLAGS += -DPROBE_TOUCH=$(PROBE_TOUCH)
endif
ifdef PROBE_TO_FASTBOOT
CFLAGS += -DPROBE_TO_FASTBOOT=$(PROBE_TO_FASTBOOT)
endif
ifdef PROBE_XNU_SECONDS
CFLAGS += -DPROBE_XNU_SECONDS=$(PROBE_XNU_SECONDS)
endif
ifeq ($(PLATFORM),qemu)
CFLAGS += -DPLATFORM_QEMU
endif
LDFLAGS := -pie --no-dynamic-linker -z norelro -z noexecstack --build-id=none -T linker.ld

LOADER ?= 0
SRCS := start.S main.c log.c uart.c timer.c fdt.c psci.c exception.c display.c wdt.c ufs.c touch.c trusty.c aoc.c dvfs.c gpu.c battery.c panel.c wifi.c lib.c
ifeq ($(LOADER),1)
SRCS += xnu.c watcher.c handoff.S
CFLAGS += -DPROBE_LOADER=1
endif
ifdef MALI_FW
SRCS += $(SRCS_FW)
endif
# WIFI_BLOB=path: the BCM4390 firmware + NVRAM (tools/wifi_blob.py) in the image, for PROBE_WIFI >= 8 (wifi_fw.S)
ifdef WIFI_BLOB
CFLAGS += -DWIFI_BLOB_PATH='"$(WIFI_BLOB)"'
SRCS += wifi_fw.S
endif
OBJS := $(patsubst %,$(OUT)/%.o,$(SRCS))

all: $(OUT)/probe.Image $(if $(filter caiman,$(PLATFORM)),$(if $(filter 0,$(LOADER)),$(OUT)/probe-boot.img))

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
