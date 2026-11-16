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

## Run 3: `probe-boot-paint.img` (b191a4e): colour bars on screen

The user confirmed the bars were visible on the phone. Probe output (grep of
the pstore log):

```
entry: EL2 SCTLR=0x30c50990 MMU=off D-cache=off I-cache=off
wdt: WTCON=0x0001af39 WTDAT=0x0000ffaf WTCNT=0x0000df3c: RUNNING, reset on expiry ON, irq off
wdt: counts at 1090 Hz (if the input clock is 24576000 Hz): period 60050 ms, 52429 ms left
paint: colour bars into 0xfac00000 (1280x2856, stride 5120)
kick: TRIG_CON=0x00003070 (hw trigger, masked) FRAME_COUNT=5
kick: TRIG_CON now 0x00003061; FRAME_COUNT 5 -> 17 in 200 ms (frames are going out: the bars should be on screen)
watchdog     : RUNNING (bootloader left it armed), period 60050 ms, 52429 ms left at probe time
finish: done; rebooting in 10000 ms (PSCI SYSTEM_RESET2 warm)
```

The next boot's bootloader reported `RST_STAT: 0x80 - SYSTEM_SWRESET_SYSTEM`,
so the probe rebooted the phone itself: the watchdog never fired, and DRAM
survived the PSCI SYSTEM_RESET2 warm reset.

| Item | Finding | Consequence |
|---|---|---|
| Pixels on glass from bare metal | **works**: write BGRA8888 into 0xfac00000, `dc civac` the lines, set `SHD_REG_UP_REQ` global+CMP (DECON0 +0x50), then clear `HW_TRIG_MASK_DECON` and set `HW_TRIG_EN` in `TRIG_CON` (+0x30) | the M3 path is proven with no DPU driver. XNU's framebuffer can be a plain buffer at 0xfac00000 once the trigger is unmasked |
| Refresh rate | 12 frames in 200 ms = **60 Hz** with the TE trigger unmasked; the bootloader had sent only 5 frames | the panel then refreshes continuously from the buffer, like video mode |
| CPU state at entry | SCTLR_EL2 = 0x30c50990: **MMU, D-cache and I-cache all off** (as the Linux boot protocol says) | the loader starts with flat physical addressing |
| Watchdog | WTCON 0x0001af39: prescaler 176, /128, ~1090 Hz. **Period ~60 s**; 52 s left when the probe ran. Reloading `WTCNT` from `WTDAT` keeps it quiet | the loader has under a minute before reset unless it kicks or stops the watchdog. XNU needs a driver or a timer that kicks it |
| Log loss in run 1 | caches were off, yet run 1's log vanished and runs 2–3 (with `dc civac` after each write) survived | likely explanation: Tensor's system-level cache (SLC) holds non-cacheable writes and drops them at reset. **Always clean to PoC before a reset**, even with the MMU off. Not proven |

## Next

M2: the stage-1 loader, based on this probe. It needs to:

- keep the watchdog fed
- drop from EL2 to EL1
- build the memory map, avoiding the image/DTB/initrd, the secure ranges and 0xfac00000
- load XNU with an Apple device tree and boot args, as HackiOS's loader does
- log to pstore
