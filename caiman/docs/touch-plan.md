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

## Step 2e: the pad CSR, from aoc.bin's A32 code, 2026-11-16

- **Where the A32 image sits in the file.** The A32 code is Thumb-2 and builds addresses with
  `movw`/`movt` pairs, so it has no literal pointers to vote on. Matching those constants against the
  string starts gives a single strong delta: AOC address = file offset + `0x3f49b5c0`. The format
  string at file 0xc1b6a7 (`GPIO %d: function %d, ...`) is therefore AOC address 0x400b6c67.
- **The `dbg gpio` handler.** It is a loadable module at file 0x10000. It reaches the main image
  through `movw`/`movt`/`bx` veneers:
  - read config: 0x40039b9f, which is `ldr r0, [0x81408000 + pad*4]`;
  - write config: 0x40039bad, which takes an array of `{pad, value}` bytes and does
    `str value, [pcu + 0x8000 + pad*4]`.
- **The config byte.**
  - bits 0-2: function;
  - bit 3: input (the console prints "input" when it is set);
  - bits 4-5: pull;
  - bits 6-7: drive strength.
- **The AP address.** The device tree's AOC node has `pcu-base = <0x1400000>` and
  `gpio-base = <0x1c00000>` in `blk_aoc` (AP 0x17000000). The firmware uses 0x81400000 (the watchdog
  at +0x3000/4/8) and 0x81c00000. So the AOC's 0x80000000 is the AP's 0x17000000, and **pad N's CSR
  is at AP 0x18408000 + 4*N**.
- **The firmware's own tables.**
  - Boot default for all 128 pads: AOC 0x401209d8. Pads 72-75 there are 0x89 (function 1).
  - Touch bus switch (0x40093932 / 0x40093974, logging "on"/"off"), a 6-pad table:
    - "on" at 0x400d97c8: 106=0x00, 62=0x08, 72..75=0x2a;
    - "off" at 0x400d97d4: all six 0x28.
  - "on" is exactly what Android's awake screen shows (step 2d). Pads 72-75 are the touch SPI.
- **`PROBE_TOUCH=2` now writes the "on" table** after `spi_init` and before the reads.
  - It reads each pad first with `safe_rd32`, and stops if the AOC block does not answer.
  - The image is `probe/out/touch11/probe-boot.img` (not run yet: the phone was off USB).

### Run 2026-11-16 (`out/touch12`, log `docs/probe-log-touch12.txt`)

- **Pad CSRs.** The CSR words at 0x18408000 answer, and the writes take. Before the writes the pads
  read 0x69 (pads 62 and 106) and 0x79 (72-75), the hardware's reset values with the AOC off.
- **The AOC's GPIO block at 0x18c00000** has the layout we guessed. Out and enable are 0 at reset.
  The inputs read `017e0000 0080060a 000001e0`. Setting out and enable for pads 87, 105 and 106
  takes: bank 2 bit 23, bank 3 bits 9 and 10.
- **The interrupt line.** The touch chip's interrupt (AP gpn0-0) was high for 3 s after reset. It
  went **low** after the AOC pad writes. This could be the chip asking to be read (its identify
  report), or just a pad change on the AOC side.
- **SPI.** Still 0 bytes received in normal mode, while loopback gives 16 of 16. The clock that
  comes back from the pads still does not reach the controller.
- **Possible cause.** In Android the pads stay on function 2 while the screen dozes and the AOC
  owns the bus (step 2d), so function 2 may mean only "the touch SPI bus". If so, the choice
  between the AP's USI2 and the AOC's own SPI master is made somewhere else (the AOC's "Change bus
  owner to AP/AOC", core 0x78xxxxxx, file 0x77e46), and that is not found yet.

### Buttons pressed by hand (2026-11-16, 22:05, `BOOT_ARGS=buttons=2`, the self-built kernel)

- **The kernel** saw every press while iOS was up. Totals: power 4, volume up 6, volume down 7.
  Each press lasted 100-370 ms, and no bus read failed.
- **backboardd** logged `[Button] VolumeIncrement page:0xC usage:0xE9` and
  `VolumeDecrement ... usage:0xEA`.
- **SpringBoard** changed the backlight with `changeSource: LockButton` (backlight-off, then on
  again with "double-click interval").
- **The panel** was dark at the end (0 of 3072 cells lit): the last power press locked the screen.

All three keys work in iOS.

### Runs 13 and 14: every pad function and every feedback delay (2026-11-16)

- **What was tried:** pads 72-75 on functions 0, 1, 2 and 3 (0x88..0x8b), each with the SPI's
  feedback clock delay (`FB_CLK`, +0x2c) set to 0, 1, 2 and 3.
- **Result:** every combination receives 0 bytes. STATUS is 0x3c0 each time: 15 of the 16 bytes are
  still in the TX FIFO, so one byte went into the shifter and the transfer stalled waiting for the
  receive side.
- **What it means:** none of the AOC pad functions on 72-75 brings the clock back.
- **What is left:**
  - the AOC-side master select (AP USI2 or the AOC's own SPI);
  - or the touch SPI is not on 72-75 at all. To check this, while Android runs touch, park
    72-75 with `dbg gpio -n N -f 0` (a write) and see whether touch stops.

### Pad 72 carries the touch bus (Android, a finger on the screen, 2026-11-16 22:38)

`tools/aoc_padtest.sh` (as root on the phone) counts the `synaptics_tcm` interrupts in
`/proc/interrupts` over three 6 s windows:

| Window | Pad 72 | Interrupts |
|---|---|---|
| A | normal | 174 |
| B | parked with `dbg gpio -n 72 -o 0 -f 0` | **0** |
| C | put back with `-f 2` | 0 |

- **C** reads 0 either because the finger had stopped or because the chip had lost its place
  mid-transfer.
- **After C:** at the screen-off right after the test, the driver reset the chip and read it
  over SPI again ("TCM Fw mode: 0x01"), so the bus works with the pad put back.

So the touch SPI does run through AOC pads 72-75 on function 2. What our boot lacks is something
besides that function. Candidates: the AOC-side master select, the pads' upper CSR bits, or AOC
clocks or power that the AOC firmware turns on.

### Run 15: aoc_req and pd_aoc

`aoc_request_on()` (aoc_core.ko's `request_aoc_on`): `aoc_req` was already 1, its status 1, and
`pd_aoc` on. The AOC is powered in our boot; still 0 bytes.

### AOC registers: Android against our boot (runs 16 and 17, 2026-11-17)

- **Reading them in Android:** the AOC console's `mem dump <addr> <bytes>` (`help -d mem`).
  - The console sends at most ~40 lines per command, so `tools/aoc_memdump.sh` sends 256-byte
    commands from a file written on the Mac.
  - Never compute addresses on the phone: Android's `sh` does 32-bit signed arithmetic. A
    sign-extended `0xffffffff81408000` reaches the console as 0xffffffff, and the AOC crashes in
    its dump loop (CONSOLE task, PC 0x78013cd4, DFAR 0xffffffff). Android restarts it within a
    second; this happened twice. `mem dump` with no address crashes the same way.
- **Reading them in our boot:** run 16 (`aoc_regs_dump()`) reads the same ranges from the AP
  (blk_aoc + 0x1400000, 11 pages; + 0x1c00000, 5 pages) after the pad writes.
- **The comparison** (`~/caiman-work/aocdiff.py`):
  - **Pad CSRs:** identical to Android after our writes.
  - **PCU pages 0x81400, 0x81401, 0x81402, 0x81404-0x81407, 0x81409:** reads abort from the AP in
    our boot. The AOC reads them in Android; 0x81409000 holds 3, 2. It is not known whether these
    pages are off in our boot or simply closed to the AP.
  - **0x81403000 (watchdog):** 0x11 in Android, 0 in ours.
  - **The GPIO block** is mirrored every 0x80 bytes. Android has these outputs:
    - 106 low and 67 high;
    - 25, 57-59, 70, 75 and 80 low;
    - 87 and 105 are not outputs, although run 13 drove them.
- **Run 17:** copies Android's output and enable words. Still 0 bytes, STATUS 0x3c0, for every pad
  function and FB_CLK. The chip's interrupt line still stays high for 3 s after reset ("no sign of
  the chip"), then drops when the AOC GPIOs are written.

So the GPIOs and the pad CSRs match Android and the bus is still dead. What is left: the PCU pages
the AP cannot read, and the AP side of the path (HSI0 pads or sysreg).
