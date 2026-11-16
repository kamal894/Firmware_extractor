# caiman: HackiOS → Pixel 9 Pro port, Phase 1 tools

Work for the build plan in [`docs/HackiOS-Pixel9Pro-caiman-Port-Build-Plan.pdf`](docs/HackiOS-Pixel9Pro-caiman-Port-Build-Plan.pdf).
This directory sits next to Firmware_extractor and does not change it; the
extractor is used only to pull stock `boot.img` / `vendor_boot.img` out of a
Pixel factory zip (see [Stock images](#stock-images)).

## caiman-probe

`probe/` builds a tiny bare-metal program (31 KB) wrapped as an Android
boot image. You `fastboot boot` it: it runs from RAM, nothing is flashed,
and it reboots back into Android by itself. It goes after the plan's two
open items without needing the debug cable:

1. **Early output.** It writes its log into the pstore/ramoops console
   buffer at `0xfd3ff000`, in the exact format Linux expects. After the
   reboot, Android shows it as `/sys/fs/pstore/console-ramoops-0`. If the
   bootloader left the debug UART on, the same log also goes out over
   serial at `0x10870000`.
2. **Boot frame buffer address.** It reads the display controller
   directly. DECON0's window registers show which DMA channel feeds each
   window, and that channel's registers hold the buffer address, size,
   format and stride. It also checks the device tree the bootloader passes
   in for the `memory-region` that Google's display driver uses for
   framebuffer handover. The bootloader adds that node at boot time, which
   is why the stored device tree didn't have it.

Along the way it records what the stage-1 loader needs to know: the
exception level it was entered at (EL2 means the loader must drop to EL1
before XNU), where the bootloader loaded the image and put the DTB and
initrd, the CPU's MIDR/MPIDR and ID registers (useful for the XNU
CPU-feature risk), `/chosen` bootargs, every reserved-memory region, the
PSCI version and reset support, and the text of the bootloader's own log
buffer at `0xfd800000`.

### Safety

- RAM only. The phone returns to Android on the next boot.
- Reads only, unless built with `PROBE_PAINT=1`, which writes colour bars
  into the frame buffer it finds.
- The riskiest step is reading MMIO registers. A powered-down block can
  hang the bus, and that can't be caught. To keep that risk small, the
  probe reads only DECON0 and the DMA channels of windows that are
  actually on. Before each risky read it writes a breadcrumb to the log,
  so a hang still tells you where it stopped. Bus faults that do raise an
  exception are caught and skipped.
- It needs an unlocked bootloader. Use a spare phone, as the plan says.

### Run it

You need Android platform-tools on the PC, and root on the phone to read
pstore.

One command does everything below. It picks the Pixel by `device:caiman`,
so an emulator or another phone can stay connected. It then boots the probe,
waits for Android to come back, and saves `probe-log.txt`:

```bash
caiman/tools/run_probe.sh
```

Variants: `caiman/tools/run_probe.sh caiman/prebuilt/probe-boot-lz4.img`,
`UART=1 caiman/tools/run_probe.sh`, and `caiman/tools/run_probe.sh --fetch`
(only reads the log, e.g. after a manual force-restart).

On Windows, run the PowerShell version from the repo folder. It takes the
same options as `-Image <file>`, `-Uart` and `-Fetch`, and needs the Google
USB driver for bootloader mode:

```powershell
powershell -ExecutionPolicy Bypass -File caiman\tools\run_probe.ps1
```

The same steps by hand follow. If adb says `more than one device/emulator`,
first run `export ANDROID_SERIAL=<serial>`, using the Pixel's serial from
`adb devices -l`.

```bash
adb reboot bootloader
```

Optional: if you have the SBU serial cable attached, turn the UART on first.

```bash
fastboot oem uart enable
```

```bash
fastboot boot caiman/prebuilt/probe-boot.img
```

After the unlocked-bootloader warning, the probe runs for a few seconds
and the phone reboots into Android by itself. Then read the log:

```bash
adb shell su -c 'ls -l /sys/fs/pstore'
```

```bash
adb shell su -c 'cat /sys/fs/pstore/console-ramoops-0' > probe-log.txt
```

The log starts with `=== caiman-probe` and ends with a `=== SUMMARY ===`
block. The lines that matter most:

```
entry        : EL2, MPIDR ..., image 0x...+0x..., DTB 0x...
fb (regs)    : 0x........ 1280x2856 fmt N bpp 4 stride NNNN (dpp N)
fb (DT)      : 0x........ size 0x...
decon mode   : video|command, N frames in 100 ms
```

### If it doesn't work

| What you see | Meaning | Next try |
|---|---|---|
| `fastboot boot` errors out | the bootloader rejected the image | `probe-boot-lz4.img`. Or rebuild with `--stock-boot` (header and OS version copied from your stock `boot.img`) |
| Stuck on a boot screen, no reboot | the probe hung (or never started) | Force a restart (hold Power until it restarts), then read pstore anyway. If the log is there, its last line shows where it hung |
| Reboots, but pstore has no `caiman-probe` text | DRAM didn't survive the reset, or the bootloader cleared it | `probe-boot-hang.img` stays in a loop instead of rebooting. Force a restart by hand, then read pstore. Or use the UART |
| `uart: transmitter disabled` | bootloader didn't enable the debug UART | `fastboot oem uart enable` before `fastboot boot` |
| `fb (regs)` shows an address outside DRAM | the display SysMMU is on, so it's an IOVA | use `fb (DT)` instead, and note it for M3 |

### Prebuilt images (`prebuilt/`)

| File | Build options |
|---|---|
| `probe-boot.img` | default: reboot 3 s after finishing (PSCI warm reset if available) |
| `probe-boot-lz4.img` | same, kernel lz4-compressed, in case the bootloader wants that |
| `probe-boot-hang.img` | `PROBE_FINISH=2`: stays in a loop at the end; you force-restart |
| `probe-boot-paint.img` | `PROBE_PAINT=1`: paints colour bars into the frame buffer, sends the frame (command-mode panel), feeds the watchdog and holds them 10 s before rebooting |

`SHA256SUMS` lists their hashes.

### Build from source

Needs `clang`, `ld.lld`, `llvm-objcopy`, `llvm-readelf`, `python3`, and
`lz4` for the lz4 variant.

```bash
make -C caiman/probe
```

Output goes to `caiman/probe/out/caiman/probe.Image` and `probe-boot.img`.
Options: `PROBE_UART=0`, `PROBE_PAINT=1`, `PROBE_FINISH=0|1|2` (warm
reboot, cold reboot, hang), `PROBE_FINISH_DELAY_MS=N`, `PROBE_CRASHTEST=1`,
and `BOOTIMG_ARGS="--compress lz4"` or `BOOTIMG_ARGS="--stock-boot path/to/boot.img"`.

The linker refuses to produce an image that would need relocations, so the
image runs wherever the bootloader loads it.

### Test in QEMU

```bash
python3 caiman/test/qemu_test.py
```

The test boots the probe on QEMU `virt` in five scenarios:

- EL2 entry with PSCI over SMC, like the Pixel
- EL1 entry with PSCI over HVC
- a device tree without the caiman nodes, which exercises the built-in fallbacks
- paint
- a deliberate crash, which exercises the exception report

The test device tree has caiman-shaped nodes: ramoops in a reserved-memory
with `#size-cells = <1>`, a bootloader log, and a DECON node with a handover
region. The display registers are faked in RAM. After the probe reboots,
the test reads guest RAM back and decodes the ramoops zone the same way
Linux does (`tools/decode_ramoops.py`). Needs `qemu-system-aarch64` and
`dtc`.

## Stock images

The packer can copy the boot header version and OS version/patch level from
your stock image, and `info` shows where the bootloader loads kernels.
Download the caiman factory zip from Google, then:

```bash
./extractor.sh caiman-<build>-factory-<hash>.zip
```

```bash
python3 caiman/tools/pack_bootimg.py info out/boot.img
```

```bash
python3 caiman/tools/pack_bootimg.py info out/vendor_boot.img
```

```bash
python3 caiman/tools/pack_bootimg.py dtb out/vendor_boot.img --out caiman-vendor.dtb
```

## Layout

```
caiman/
  probe/        bare-metal probe (start.S entry + Image header, exception vectors,
                ramoops/UART logging, FDT walker, PSCI, display probe)
  tools/        run_probe.sh / run_probe.ps1 (boot the probe + fetch its log), pack_bootimg.py
                (pack/info/dtb), decode_ramoops.py, check_image.py
  test/         qemu_test.py
  prebuilt/     ready-to-boot images
  reference/    Google's caimito/zumapro DTS and display-driver files the
                addresses come from (see reference/google/SOURCES.md)
  docs/         the project build plan
```

## Status

- QEMU: all five scenarios pass.
- Pixel 9 Pro: **the probe runs**. Results are in
  [`docs/hardware-results.md`](docs/hardware-results.md):
  - entry at EL2, image at 0x80000000, DTB at 0x8a000000
  - frame buffer at 0xfac00000 (1280×2856, BGRA8888), command-mode panel
  - the bootloader leaves the watchdog running
  - the pstore log works

  - `probe-boot-paint.img` puts colour bars on the screen (frame trigger works)
  - the watchdog period is ~60 s

  Next: the M2 stage-1 loader ([plan](docs/m2-loader-plan.md)).

When the log comes back, record the frame buffer address and the entry EL
in `probe/platform_caiman.h` and in the plan's progress log. This probe is
also the start of the stage-1 loader (plan Option A): it already has the
entry point, exception vectors, logging and DT parsing that the loader needs.
