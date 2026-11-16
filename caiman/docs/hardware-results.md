# caiman-probe on real hardware

Pixel 9 Pro (caiman), bootloader `ripcurrentpro-17.0-15199480` (LK, built
Apr 14 2026), run from a Mac with `caiman/tools/run_probe.sh`.

## Run 1: `probe-boot.img` (e23be95)

`fastboot boot` succeeded and the phone came back to Android, but pstore
held only Android's previous console log (52 KB) and no probe text. DRAM
evidently survives resets on this phone: that Android log outlived two
restarts. So either the probe didn't run, or its writes stayed in a dirty
cache line. acd5edb added `dc civac` after every log write.

## Run 2: `probe-boot-hang.img` (acd5edb): the probe runs

Full output: [`hw-run2-output.txt`](hw-run2-output.txt). Summary:

```
entry        : EL2, MPIDR 0x81000000, image 0x80000000+0x1f440, DTB 0x8a000000
dram         : 8 range(s) from DT
ramoops log  : console zone 0xfd3ff000 size 0x200000
uart         : not usable
psci         : version 0x10001, SYSTEM_RESET2 yes
fb (regs)    : 0xfac00000 1280x2856 fmt 0 bpp 4 stride 5120 (dpp 0)
fb (DT)      : no handover memory-region
decon mode   : command, 0 frames in 100 ms
aborted reads: 0
```

What this establishes:

| Item | Finding | Consequence for the port |
|---|---|---|
| Our code runs | yes, from RAM via `fastboot boot` with a plain (uncompressed) arm64 Image in a v4 boot image | **M1 gate met**, via pstore instead of serial |
| Early output | the ramoops console zone at 0xfd3ff000 (2 MiB) works. On the next boot the bootloader **appends its own log** to the same buffer | the main debug channel from now on, no cable needed |
| Entry state | **EL2**, boot CPU MPIDR 0x81000000 (core 0, MT bit set: affinity in Aff1, as the plan said) | the stage-1 loader must drop EL2 → EL1 before XNU |
| Load addresses | image at **0x80000000**, DTB at **0x8a000000**, initrd 0x86000000–0x87f32125 | free RAM for XNU and its ramdisk must avoid these and the regions below |
| Secure memory | bootloader log: secure DRAM **0xb6200000 + 0x9e00000**, secure page tables **0xe0000000 + 0x3800000** | never touch: these go in the loader's memory map as reserved |
| PSCI | 1.1 over SMC, SYSTEM_RESET2 supported | CPU on/off and reset for XNU's platform code |
| Frame buffer | **0xfac00000**, 1280×2856, BGRA8888, stride 5120, fed by DMA channel L0 (dpp 0) into DECON window 5. No `memory-region` handover node in this boot | the address the draft `platform_caiman.h` guessed from GS101 is confirmed |
| Display mode | DECON0 in **command mode, hw (TE) trigger**, DSC on (two encoders), single DSI to panel `google-cm4` (id 0x22a8a00a). No frames go out while idle | writing pixels alone shows nothing: every update needs a frame trigger (or the TE trigger left unmasked) |
| Watchdog | the bootloader leaves the **cluster-0 watchdog armed**: the hung probe was reset ("APC Watchdog Early", RST_STAT CLUSTER0_NONCPU_WDTRESET) and DRAM survived | the loader (and later XNU) must kick or stop it: `watchdog_cl0@10060000` |
| UART | TX not enabled ("console is disabled") | optional; `fastboot oem uart enable` + cable if ever needed |
| SoC | Product ID 0x09875 (Tensor G4) rev A1, 16 GiB Samsung DDR, MTE off | |

## Next: `probe-boot-paint.img`

Paints colour bars into 0xfac00000. It then does what the display driver
does to send a frame in command mode: a shadow update request, then
unmasking the TE trigger. It keeps the watchdog fed for 10 s so the bars
stay up, then reboots. Expected log lines:

```
paint: colour bars into 0xfac00000 (1280x2856, stride 5120)
kick: TRIG_CON=0x........ (hw trigger, masked) FRAME_COUNT=N
kick: TRIG_CON now 0x........; FRAME_COUNT N -> M in 200 ms (frames are going out ...)
wdt: WTCON=... RUNNING ...
```

Bars on screen means **M3's display path works from bare metal**: same
buffer, same trigger, no DPU driver needed.
