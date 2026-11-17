# Touch screen plan (M5)

The Pixel 9 Pro's touch screen, as Android's own device tree describes it (`/sys/firmware/fdt`):

| | |
|---|---|
| chip | Synaptics TCM (`synaptics,tcm-spi`), `spi@111D0000` (Android's spi20), 10 MHz, mode 0 |
| power | LDO4M of S2MPG14 (`L4M_TS_AVDD`, 3.3 V) and LDO25M (`L25M_TSP_DVDD`, 1.8 V); power-delay 200 ms |
| reset | `gpp1-1` (GPIO_PERIC0 0x10840000), active low; 2 ms pulse, then 50 ms |
| interrupt | `gpn0-0` (0x15060000), active low |

HackiOS's kernel has the touch plumbing (`/dev/hakios-touch`), but its drivers are Goodix and
FocalTech on Qualcomm SPI: a Synaptics TCM driver on the Tensor USI SPI is new work, in a kernel
built here.

## Step 1: what the bootloader leaves (`PROBE_TOUCH=1`, read-only), 2026-11-16

The PMIC is reached only through ACPM (mailbox 0x15110000, SRAM 0x15700000, `initdata` at +0xa000;
`s2mpg14mfd` is IPC channel 2, speedy channel 0), the way Google's `acpm_ipc.c`/`acpm_mfd.c` do
(`probe/touch.c`). The `*_front`/`*_rear` fields of a channel are SRAM offsets of the counters (a
first try read them as counters and stopped at a write outside the SRAM; nothing came of it).

```
touch: ACPM initdata: 8 plugins, ipc_channels at 0x8f20, 41 channels, 16 for the AP
touch: ACPM channel 2: id 2, queue 5 x 16 bytes, AP polls 1; to APM front 4 rear 4 (at 0x3c2ec), from APM front 4 rear 4
touch: S2MPG14 chip id 0x10 (the bootloader read 0x10)
touch: LDO4M  (TS_AVDD 3.3 V)  CTRL 0x3c: off
touch: LDO25M (TSP_DVDD 1.8 V) CTRL 0x2c: off
touch: reset pin 1: function 0, level 0, pull 1
touch: interrupt pin 0: function 0, level 0, pull 1
touch: SPI controller @ 0x111d0000: all zero
```

The touch chip is unpowered. Its supplies already hold the right voltages (LDO4M group 5:
1.8 V + 0x3c x 25 mV = 3.3 V; LDO25M group 4: 0.7 V + 0x2c x 25 mV = 1.8 V), so powering it is
setting the enable bits 7:6: `0x3c -> 0xfc`, `0x2c -> 0xec` (FUNC_WRITE through ACPM).

## Step 2: power and reset (`PROBE_TOUCH=2`, with the owner's go-ahead), 2026-11-16

Two PMIC writes through ACPM (FUNC_WRITE: the enable bits only, and only if the voltages read back
as 0x3c/0x2c), the reset pin held low meanwhile, 200 ms, then the reset released; both pins without
pull as Android's `ts-irq`/`ts-reset`:

```
touch: LDO4M  CTRL now 0xfc: ON
touch: LDO25M CTRL now 0xec: ON
touch: interrupt (powered, in reset) pin 0: function 0, level 1, pull 0
touch: interrupt line stayed high for 1 s after reset: no sign of the chip
touch: reset (after) pin 1: function 1, level 1, pull 0
```

The interrupt line went from 0 (unpowered) to 1 with the supplies on: the chip side of the line is
powered. It did not pull it low within 1 s of the reset; a TCM chip may wait for the host to read
before it signals again, so the next answer has to come over SPI.

## Step 2b: the SPI bus (not moving yet), 2026-11-16

`spi@111D0000` is BLK_HSI0's USI2 (`zumapro-usi.dtsi`: clocks `VDOUT_CLK_HSI0_USI2_USI`,
`GATE_HSI0_USI2_USI`; Android's node has an empty `pinctrl-0`, and no pin controller of HSI0 shows up in
Android's debugfs). The probe sets it up as Google's `spi-s3c64xx.c` and mainline `exynos-usi.c` do:
`sysreg_hsi0` + 0x101c = 2 (SPI; the write takes), `USI_OPTION` CLKREQ_ON, `USI_CON` out of reset,
master, mode 0, 8 bits, 64-byte FIFO, polled, chip select by hand. CMU_HSI0's USI2 gates (zuma CAL
offsets 0x2120/0x20fc/0x2100/0x20bc) read `0x00200000` (clock value 1) and were forced on manually too.

The controller keeps what is written to it, but a 48-byte read never runs: `STATUS 0xbc0`, 47 bytes left
in the TX FIFO, none received. One byte went into the shifter and no clock edges followed.

What it rules out and leaves:
- not the register clock, the USI mode, the USI reset, `CLKREQ_ON`, or the gates as zuma names them;
- left: the USI2 functional clock's source (mux `0x101c` and divider `0x181c` read 0: zuma offsets,
  unchecked for Tensor G4), and the pins (none muxed by Android: dedicated pads, or set up by firmware).

The chip itself: once powered, its interrupt line goes high, and a few seconds after the reset it pulls
it low (seen after the SPI attempts): it has a report waiting.

### The clock is fine: it is the pins

This phone's own clock tables are in `cmupmucal.ko` of the `vendor_kernel_boot` ramdisk: USI2's mux
(0x101c) and divider (0x181c) are at zuma's offsets, and it has a Q-channel, `QCH_CON_USI2_HSI0_QCH` at
CMU_HSI0 + 0x30cc (ENABLE bit 0, CLOCK_REQ bit 1), which reads `0x2`: clock requested, held on.

The controller's internal loopback (`MODE_CFG` bit 3) settles it:

```
touch: SPI loopback: 16 of 16 bytes back (ff ff ...), STATUS 0x02000000: the clock runs: the pins are the problem
```

In normal mode the receive side samples on the clock fed back from the pads, which are not muxed to
the USI. Android's `spi@111D0000` has an empty `pinctrl-0`, and HSI0 has no GPIO block of its own:
its USIs reach PERIC0/1 pads as a pin function (`hsi2c@111F0000`, HSI0's USI4, uses `spi1_bus`,
`gpp2-0..2`, function 3, the same function the PERIC USIs' own bus groups use). Which pads carry HSI0's
USI2 is not in Android's device tree: something else (the AOC firmware, which the touch offload
`goog,offload-enabled` uses, or the bootloader) must set them.

### The pads, from Google's own device-tree source

A scan of every 4-pin USI pad group of PERIC0/1 (`uartN_bus_single`: gpp2, 4, 6, 8, 10, 12, 16,
19-0, 19-4, 20, 21-0, 21-4, 22, 23) with functions 2, 3 and 4, each restored after its try, found no
group where the clock gets out. Google's source for this phone's touch
(`kernel/devices/google/caimito`, `dts/zuma-caimito-common-touch.dtsi`) says why:

```
/* Touch is connected to spi_20, pins:
 *   CLK=GPB10[4], MOSI=GPB10[5], MISO=GPB10[6], CS=GPB10[7]
 * TS1_RESET_L = XAPC_USI11_RTSn_DI = GPP1[1] = <&gpp1 1 0>
 * TS1_INT_L = AOC_APM_TS1_INT_L_R1 / XAOC_GPIO0 = GPN0[0] = <&gpn0 0 0>
...
&spi_20 { ... pinctrl-0 = <>; /* TODO: pinctrl */
```

GPB10 is not a bank of any pin controller Android registers on this phone (the bank tables in
`pinctrl-exynos-gs.ko` have gpb0 and gpb1 only, for other SoCs), and Google's own DT leaves the SPI's
pinctrl as a TODO: the pads are set up somewhere else, or need nothing on a normal boot.

### Where GPB10 is not

- HSI0's address range: the 64 KiB slots no node claims either abort or hold other devices'
  registers (`PROBE_TOUCH=3`, read-only); no GPIO block among them.
- This phone's pin bank table (from `pinctrl-exynos-gs.ko`, the zumapro table: PERIC0 gpp0..gpp16
  with gpp17 at 0x200 and gpp16 at 0x220, PERIC1 gpp19/20/21/24/22/23 at 0x00..0xa0): no gpb bank,
  and gpp10 has 4 pins, so "GPB10[4..7]" is not a typo for gpp10.
- Pixel 8's touch (`shusky`, `zuma-shiba-touch.dtsi`, `zuma-husky-touch.dtsi`) is on `spi_20` too,
  with no pins either.

So on both phones Linux never touches these pads: they are HSI0 USI2's own pads, not muxed GPIOs,
which should work as they are -- and in loopback the controller does. What stops the pad side may be
the pads' I/O supply or a pad-level enable that the normal boot sets and this boot path does not.

### The bus belongs to the AOC

Not the power domain: Google's own sequence for it (`hsi0_on` in `cmupmucal.ko`: PMU 0x15460000 +
0x2a80 bit 0, wait for +0x2a84) is one write, and HSI0 is on. The PMU tables have no pad retention for
it either.

The touch DT has `tbn { compatible = "touch_bus_negotiator"; tbn,mode = <2>; }`. In Google's
`kernel/google-modules/touch/common/touch_bus_negotiator.c`, mode 2 is `TBN_MODE_AOC_CHANNEL`: the
touch SPI is shared between the AP and the AOC (the always-on core, for touch while the AP sleeps),
and the AP asks the AOC firmware for the bus over an AOC service channel before every use. The pads
named in the DT comment ("GPB10[4..7]") are not an AP pin bank: they are on the AOC's side (the AOC
firmware has its own GPIO and pad tables, "`pin_configuration.h enum zuma_pad_name`").

So in this boot, where nothing has started the AOC, the pads are not given to USI2, and the AP's
controller clocks into nothing. Loopback works because it never leaves the controller.

## Next

2c. Either set the AOC pad mux for these four pads ourselves (its register is in the AOC firmware,
    an Xtensa image, `/vendor/firmware/aoc.bin`: reverse work), or start the AOC and use the TBN
    channel as Android does (much more). Superseded: the pads' power and enables (PMU pad retention/isolation for HSI0, the I/O rail) -- or watch
    what Android's kernel does to HSI0 USI2 at boot. Superseded: find GPB10's registers (a GPIO block of HSI0 that Linux does not register: read-only look at the
    blocks around 0x11000000-0x111fffff), put pins 4-7 on the USI, and read the identify report.
    Earlier idea, now settled: HSI0 USI2's pads (by reading the GPIO banks while Android runs the touch screen, or from the
    AOC side), set them, and read the identify report. Earlier idea, now settled: the USI2 clock source (its CAL tables are not in the public google-modules
    tree; Android's kernel does not expose `clk_summary` or `/dev/mem`), or the pads (the two PMIC writes above), reset it, and read its identify report over SPI
   (USI SPI: clocks, USI mode, FIFO, polled).
3. A kernel driver feeding `/dev/hakios-touch`, in a HackiOS kernel built on the Mac.

## Buttons first (2026-11-16)

Android's `gpio_keys`: power `gpa4-0`, volume down `gpa4-1`, volume up `gpa6-2`, all on GPIO_ALIVE
(0x154d0000; gpa4 at +0x80, gpa6 at +0xa0 in this phone's pinctrl table), active low, function 0xf
(external interrupt) under Android. `PROBE_TOUCH=4` reads them (read-only) for 20 s:

```
touch: buttons: gpa4 CON 0x00000000 DAT 0x00000003 PUD 0x00000033; gpa6 CON 0x00000000 DAT 0x0000002f PUD 0x00010013
touch: buttons at 0 ms: power up, volume down up, volume up up
```

All three read released, pulled up, as they should. (No presses were made in that run.)

Getting them to iOS needs kernel code: `/dev/hakios-touch`'s reader sleeps until a driver calls
`wakeup()`, so another core writing into the queue would not be seen, and HackiOS's
`hakios.buttons.c` is the POCO X5 Pro's (Qualcomm PMIC over SPMI). A caiman driver that polls these
three bits is small; it needs the kernel built here (`xnu-deps/setup.sh`, `xnu/apply.sh`,
`xnu-build.sh`).

### The kernel built here, with the buttons (2026-11-16)

The SDKs come from Xcode 27.2 (App Store), not from the mirrors `xnu-deps/setup.sh` downloads by
default. `hakios/caiman-kernel.patch` (on HackiOS 63fc2b4) adds `XNU_XCODE_SDKS=1` to the setup
script: Xcode's iPhoneOS SDK stands in for 16.5, and the DriverKit headers are copied (not linked:
`host/bin/iig` does not follow links) from Xcode's macOS SDK. It also adds `buttons=2` to
`hakios.buttons.c`: the three GPIO_ALIVE bits above, read every 16 ms, read-only.

```
cd hakios-xnu-kernel && git apply ../caiman/hakios/caiman-kernel.patch
XNU_XCODE_SDKS=1 xnu-deps/setup.sh --stage1 && xnu/apply.sh && XNU_XCODE_SDKS=1 xnu-deps/setup.sh --stage2
./xnu-build.sh -j8 -k
BOOT_ARGS=buttons=2 caiman/tools/run_darwin.sh LOADER \
    xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 userland/root.apfs
```

The build stops at the kext symbol sets (`check_all_exports`): Xcode 27's DriverKit headers give
the RPC methods other signatures than the 13.3 export lists name. The kernel itself is linked by then,
and nothing here loads kexts, so `mach.development.bcm2837` is used as it is.

That kernel boots on the phone like the prebuilt one (same home screen, 1320 of 3072 cells lit):

```
Darwin Kernel Version 22.5.0: Mon 16 Nov 2026 18:00:31 CST; ...xnu-8796.121.2-dirty
buttons: caiman: gpa4 CON 0 DAT 0x3, gpa6 CON 0 DAT 0x2d
buttons: caiman: watching power volume-up volume-down, every 16 ms: power is the side button
hakios: input: touch screen service 0x10075cd90, buttons 0x100760f90
buttons: so far 0 presses of power, 0 of volume up, 0 of volume down; now none held; 3750 reads ...
```

No key was pressed in that run.

With `BOOT_ARGS="buttons=2 press_power=100"` the kernel itself posts two short power presses, ten
seconds apart (HackiOS's test, through the same `hakios_touch_post` as the driver):

```
buttons: press_power: power PRESSED (by the test) at 280.643 s (00:15:11.122 UTC)
buttons: press_power: power released (by the test) at 280.794 s (00:15:11.273 UTC)
buttons: press_power: power PRESSED (by the test) at 290.795 s (00:15:21.274 UTC)
buttons: press_power: power released (by the test) at 290.946 s (00:15:21.425 UTC)
screen: 241 of 3072 cells lit
```

The first press locked the phone, the second woke it: the panel shows the lock screen (clock and
date), not the home screen (1320 cells before). So a key record reaches backboardd and SpringBoard
acts on it. What is left is a run with a hand on the real keys.

## Step 2d: asking the AOC itself (Android running, touch working), 2026-11-16

Android has the AOC's debug console at `/dev/acd-debug` (one opener at a time; write and read on
one descriptor: `exec 3<>/dev/acd-debug`). `help` lists, on core 1 ("A3", the A32), among others
`dbg gpio`: "Sets a gpio as a specified mux setting, and direction ... `-d` dumps the csr register,
`-a` dumps all", with pad numbers from `pin_configuration.h enum zuma_pad_name` (39 = XAOC_GPIO2).

`dbg gpio -n N -d 1` for N = 0..127 (read only) gives every pad's function, direction and level:
`docs/aoc-pads-android.txt`. Four pads side by side on function 2 stand out (72-75, all inputs
reading 1), and the 4-pad groups 8-11 and 16-19 look like SPI buses (three outputs, drive 2, and
one input). The AOC's numbering is not Linux's: Linux (debugfs, mountable as root:
`mount -t debugfs debugfs /sys/kernel/debug`) has gpn0-0 = pin 118, and AP `CON` values like
0xf (EINT) never appear in the AOC's list.

Screen awake vs dozing (pads 60-80): only pad 64 changes, function 1 awake, 2 dozing. This is the
AOC taking something over while the AP sleeps. One pad, not the four SPI pads.

**Caution:** `mem dump` without arguments crashed the AOC (it was reset at 17:35:12, right after that
command; Android restarted it in 0.2 s, `restart_count` 1, touch and Bluetooth came back). Only
`help` and `dbg gpio ... -d`/`-a` are known to be safe.

Next: the pad CSR's address (the `dbg gpio` handler on the A32 core of `aoc.bin`: the image is a
container, the A32 part's load address is not found yet), so that the loader can put the touch pads
on function 2 itself.
