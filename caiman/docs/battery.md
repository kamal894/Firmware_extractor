# Battery (MAX77779)

The Pixel 9 Pro's battery is managed by a Maxim MAX77779 on `hsi2c_6` (0x108d0000, USI v2, the DT's
`samsung,exynos5-hsi2c`): fuel gauge at 0x36 (`maxim,max77779fg-i2c`), charger at 0x69, PMIC at 0x66.

## How iOS gets it

1. **Loader** (`probe/battery.c`, `PROBE_BATTERY=1`): reads, never writes, the gauge and the charger over
   hsi2c_6 as the bootloader left it (CTL 0x48, CONF 0x980110ff, FS timing set): polled transfers as Linux's
   `i2c-exynos5.c` does in `HSI2C_POLLING` mode. Once at boot (a log line), then every 5 s from the watcher core.
2. It fills HackiOS's `struct loader_battery` (the record another phone's loader shares) plus six fields of its
   own (SOC in hundredths, RepCap / FullCapRep / DesignCap in mAh, cycles, temperature in hundredths), in a
   16 KiB page of the loader's, and puts `battery=1 battery_shared_pa=0x...` in the boot-args.
3. **Kernel** (`drivers/power/hakios.battery.c`): reads the page every 5 s; with the extra fields it calls
   `hakios_battery_report()`, which makes the `IOPMPowerSource` (`HakiosBattery`, `hakios.battery.cpp`) that
   powerd, the status bar and Settings > Battery read.

## Registers and units

From Google's `max77779_v20240115_regs.h`, `max77779_fg.c` and `maxfg_common.h` (kernel/google-modules/bms):

| What | Register | Unit |
|---|---|---|
| RepSOC | FG 0x07 | 1/256 % |
| RepCap, FullCapRep, DesignCap | FG 0x06, 0x10, 0x18 | 5 uVh / Rsense (Rsense 200 x 10 uOhm: the driver's default, the DT gives none) |
| VCell | FG 0x1a | 78.125 uV |
| Current | FG 0x1c | 1.5625 uV / Rsense, out of the battery positive (the driver reports -Current) |
| Temp | FG 0x1b | 1/256 C |
| Cycles | FG 0x17 | 1 % of a cycle |
| DevName | FG 0x21 | 0x5100 here |
| CHG_INT_OK | CHG 0xb6 | bit 6 CHGIN_OK, bit 5 WCIN_OK |
| CHG_DETAILS_01 | CHG 0xb8 | CHG_DTLS bits 3:0: 0 pre, 1 fast CC, 2 fast CV, 3 top-off, 4 done |

## Checked against Android (2026-11-18)

| | Android `dumpsys battery` | Loader (ws55) |
|---|---|---|
| Level | 100 (UI) | RepSOC 96.09 % |
| Voltage | 4405 mV | 4392 mV |
| Temperature | 35.9 C | 35.26 C |
| State | charging (AC) | CHG_DTLS 3 (top-off), +442 mA |

Android's level is the UI's scaled one (it shows 100 above ~95 % RepSOC); the gauge's own is what is shared.

Not done: anything written to the charger (limits, JEITA are the bootloader's settings, left as they are).
