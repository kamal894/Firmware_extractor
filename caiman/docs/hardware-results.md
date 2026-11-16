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

## Run 4: `xnu-boot.img` (ce41c6f): **XNU runs on the Pixel 9 Pro (M2)**

The loader (`make LOADER=1`) probed as usual, then started HackiOS's prebuilt XNU (see
`prebuilt/NOTICE`). The output, as `run_probe.sh` printed it:

```
xnu: kernel 16809112 bytes, ramdisk 0 bytes, XNU memory 0x880000000 + 0x100000000
xnu: console -> loader_log (vmapple_uart transmit patched, receive off)
xnu: entry 0xfffffff0074424a8 (pa 0x8834424a8), top 0x884408000; loader_log found
xnu: device tree 0x884408000 (11740 bytes), boot_args 0x88440c000, top 0x884410000
xnu: uart for the kernel 0x8001c000 (a page of RAM: the console goes only to loader_log), framebuffer 0xfac00000
xnu: boot-args: debug=0x8 serial=3 cpus=1 -v
kick: TRIG_CON now 0x00003061; FRAME_COUNT 5 -> 17 in 200 ms (frames are going out: the bars should be on screen)
watcher: PSCI CPU_ON(0x100) = 0watcher: running on MPIDR 0x81000100 at EL2; XNU gets 45 s

=== XNU after 45 s (last look): loader_log 5109 bytes ===
Darwin Kernel Version 22.5.0: Sun Oct 25 22:52:46 PDT 2026; shimoku:xnu-8796.121.2[xnu-8796.121.2-dirty]/BUILD/obj/DEVELOPMENT_ARM64_BCM2837
pmap_startup() init/release time: 20508 microsec
vm_page_bootstrap: 256747 free pages, 5397 wired pages, (up to 0 of which are delayed free)
...
"vm_compressor_mode" is 2
VM bootstrap done: 90 maps, 163 entries and 500 holes left
...
Serial requested, consistent debug disabled or debug boot arg not present, configuring debugging over serial
iBoot version: iBoot-8422.100.650
IONVRAM.cpp:initImageFormat:1364 - NVRAM size is 8192 bytes
minicrypto: registered (md5, sha1, sha2, hmac, aes-ecb/cbc/ctr, hmac-drbg prng); no xts/gcm, no RSA
Security policy loaded: Guarded vnode policy (vnguard)
Security policy loaded: Apple Mobile File Integrity (stand-in) (AMFI)
Security policy loaded: Seatbelt sandbox policy (stand-in) (Sandbox)
GenericARMPE: platform "device-tree"
GenericARMCPU: cpu 0 (phys id 0x0) registered
MAC Framework successfully initialized
mcache: 1 CPU(s), 64 bytes CPU cache line size
mbinit: done [96 MB total pool size, (64/32) split]
dlil_init: All the created dlil kernel threads have been scheduled at least once. Proceeding.
ifnet_attach: All kernel threads created for interface lo0 have been scheduled at least once. Proceeding.
skip attaching fsw to lo0 using legacy TX modelMiniIOMobileFramebuffer: display 1125x2436 on a panel of 1280x2856 (5120 bytes per row)
GenericARMPE: the slower cores started too: 1 processors running
!BSD
Waiting on <dict ID="0"><key>IOProviderClass</key><string ID="1">IOMedia</string><key>Content</key><string ID="2">Apple_HFS</string></dict>

=== end of XNU snapshot ===
watcher: 45 s are up; resetting the phone
```

The next boot's bootloader reported `RST_STAT: 0x80 - SYSTEM_SWRESET_SYSTEM`: the watcher's PSCI
reset, not the watchdog. Its PSCI breadcrumbs show `Core1: 0000480f`, the watcher core.

What this establishes:

- **M2 gate met**: XNU's own log, from the Pixel, read back through pstore.
- The EL2 → EL1 hand-off, the Apple device tree (GICv3 at 0x10400000/0x10440000, cpu0 at Aff 0),
  and XNU's window in the high bank (0x880000000, 4 GiB across two of the DT's 2 GiB ranges) work
  as written.
- The kernel ran its threads (`dlil_init ... scheduled at least once. Proceeding`), so the
  scheduler, the arch timer and GIC interrupts work on Tensor G4.
- It is the same log, line for line, as the QEMU `xnu` scenario, which stops at the same place:
  waiting for a root disk.
- The watcher on core 1 (PSCI CPU_ON from EL2) kept the watchdog fed, copied the log and reset
  the phone on time.

## Next

- M3: confirm what the `-v` text console looked like on the panel (the TE trigger was unmasked
  before the jump).
- M4: give the kernel a root disk. First an Apple-free one (HackiOS's `hello`/`init`), which should
  print "Hello from user space on Darwin! I am process 1". Then iOS 16.5's own user space from your
  IPSW.

## Earlier plan

M2: the stage-1 loader, based on this probe. It needs to:

- keep the watchdog fed
- drop from EL2 to EL1
- build the memory map, avoiding the image/DTB/initrd, the secure ranges and 0xfac00000
- load XNU with an Apple device tree and boot args, as HackiOS's loader does
- log to pstore
