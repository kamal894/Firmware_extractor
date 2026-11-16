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

## Next

2c. Find HSI0 USI2's pads (by reading the GPIO banks while Android runs the touch screen, or from the
    AOC side), set them, and read the identify report. Earlier idea, now settled: the USI2 clock source (its CAL tables are not in the public google-modules
    tree; Android's kernel does not expose `clk_summary` or `/dev/mem`), or the pads (the two PMIC writes above), reset it, and read its identify report over SPI
   (USI SPI: clocks, USI mode, FIFO, polled).
3. A kernel driver feeding `/dev/hakios-touch`, in a HackiOS kernel built on the Mac.
