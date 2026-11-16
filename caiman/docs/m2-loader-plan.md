# M2 plan: a loader that starts XNU on the Pixel 9 Pro

Goal (plan's M2): XNU prints its first log lines on caiman, read back from pstore.

## What HackiOS gives us (read 2026-11-16)

- **One kernel for every device.** `hackios-xnu-kernel/prebuilt/hakios-kernel` is a Mach-O built as
  `DEVELOPMENT_ARM64_BCM2837` (xnu-8796.121.2, 25 Oct 2026). The same binary runs the QEMU target and
  the POCO X5 Pro. Drivers are picked by boot-args and the device tree, so M2 needs **no kernel build**.
- **Identity.** `xnuloader/mkidentity.sh --placeholder` makes an `identity.h` with no Apple data,
  "enough to boot the open kernel" (no iOS userland). M2 uses the placeholder. M4 onwards needs your
  own iOS 16.5 IPSW (`DeviceTree.d22ap.im4p` from it).
- **Payload format "XNUP"** (`xnuloader/mkpayload.py`): `u32 magic, u32 size`, then entries
  `char name[16], u32 size, u32 reserved, data padded to 16`, ending with an empty name. It carries the
  kernel, the ramdisk and options.
- **The template: `hakios-mt6768/mtkloader`** (MIT). This is a bare-metal loader for a non-UEFI phone,
  which is exactly our situation. Its `run_xnu()`:
  1. parses the payload and copies the Mach-O segments to `XNU_PHYS_BASE` (one contiguous free RAM
     range; slide = `XNU_PHYS_BASE - 0xfffffff004000000`)
  2. finds kernel symbols and patches a few:
     - the PL011 UART driver's `transmit_ready`/`transmit_data` go to `loader_log_putc`, so the
       kernel's text lands in its own log buffer
     - GICv3 EOImode is fixed for every core
     - a "return to loader" stub (`XNULDRST`) catches panics
  3. places the ramdisk after the kernel, then builds the Apple device tree:
     - root identity, `chosen` (dram-base/size, psci-method, nvram…), `memory-map`
     - `cpus`, with `reg = core << 8`: affinity in Aff1, the same as Tensor
     - `arm-io` with `gic` (arm,gic-v3) and `uart0`
  4. fills `boot_args` (rev 2) with `physBase`, `memSize`, the device tree, the framebuffer in
     `video`, and the command line (`debug=0x8 serial=3 rd=md0 cpus=N …`)
  5. starts a **watcher** on a spare core (PSCI CPU_ON). It snapshots XNU's log and resets the phone
     after N seconds. Then it jumps to the kernel at EL1 with x0 = boot_args.

## What changes for caiman (from runs 2–3, docs/hardware-results.md)

| mtkloader (MT6768) | caiman loader |
|---|---|
| chained in `boot_a`, flashed | `fastboot boot` only. Boot image kernel = loader + XNUP payload appended (the probe's `pack_bootimg.py`) |
| LK enters at EL1 | **EL2 entry**: drop to EL1 before XNU. That means HCR_EL2.RW=1 with no traps, CNTHCTL_EL2 EL1PCTEN/EL1PCEN, CNTVOFF_EL2=0, CPTR_EL2 clear, ICC_SRE_EL2 SRE+Enable, VTTBR_EL2=0, then SPSR_EL2=EL1h with DAIF masked and ERET |
| video-mode panel at 0x787b0000 | **command-mode** panel at **0xfac00000** 1280×2856 BGRA8888 stride 5120. Unmask the DECON TE trigger before the jump (proven in run 3), so whatever XNU draws is refreshed at 60 Hz |
| MediaTek TOPRGU watchdog armed by the loader | **cluster-0 watchdog already armed (~60 s)**. The watcher core kicks it (`WTCNT = WTDAT`) while XNU runs and stops kicking after N seconds |
| marker + reboot into the loader to save the log | fastboot-booted, so the next boot is Android. The watcher therefore **copies XNU's log buffer into the pstore console zone** (with `dc civac`) every second. After the reset, `run_probe.sh --fetch` reads it |
| UART at MediaTek address | UART not enabled by the bootloader. The kernel log goes only to its buffer and pstore |
| 4 cores, watcher on core 7 | start with `cpus=1` (CPU 0, A520). The watcher on another core (e.g. Aff1=1) |
| `XNU_PHYS_BASE` 0xc0000000 | **to choose** from the DT memory map, avoiding: the loader 0x80000000, DTB 0x8a000000, initrd 0x86000000–0x87f32125, secure 0xb6200000+0x9e00000 and 0xe0000000+0x3800000, fb 0xfac00000, ramoops 0xfd3ff000, bldr log 0xfd800000. Probably the high bank above 4 GiB. **Needs the `dt: memory` / `dt: rsv` lines of run 2's log** |

## Steps

1. **Memory map.** Get the DT memory ranges from run 2's `probe-log.txt` and pick `XNU_PHYS_BASE` and `memSize`.
2. **`caiman/loader/`**, grown from the probe (entry, exception vectors, pstore log, FDT reader,
   PSCI, watchdog, display trigger) plus ports of mtkloader's payload parser, Mach-O loader,
   symbol finder and patches, Apple device-tree writer and `boot_args`. Add the EL2→EL1 drop and the
   watcher core. Keep mtkloader's MIT notice on the ported parts.
3. **QEMU first.** Boot the prebuilt kernel with this loader on QEMU `virt` (GICv3, EL2 entry, PSCI SMC,
   like the Pixel), placeholder identity, `cpus=1`. Success means XNU's log appears in the decoded
   pstore zone. HackiOS's QEMU target runs the same kernel, so a failure here is the loader's.
4. **Phone.** The same image with caiman's addresses, through `run_probe.sh`. Gate: XNU log lines in
   pstore (M2). With the TE trigger unmasked and `-v`, the kernel's text console should also appear on
   the screen (early M3).

## Not needed yet

The iOS IPSW and system image (M4), touch/buttons/storage drivers (M6), and the SBU serial cable.
