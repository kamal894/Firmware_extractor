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

## Next

2. Power the chip (the two PMIC writes above), reset it, and read its identify report over SPI
   (USI SPI: clocks, USI mode, FIFO, polled).
3. A kernel driver feeding `/dev/hakios-touch`, in a HackiOS kernel built on the Mac.
