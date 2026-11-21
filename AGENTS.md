# iOS on the Pixel 9 Pro ("caiman") — context for Codex

The owner asks questions in Majhi Punjabi (Gurmukhi or romanized). Answer in Punjabi, simply, and name exact files.

## What this is
Booting iOS 16.5 (20F66) user space on a Google Pixel 9 Pro (caiman, Tensor G4 / zuma, serial 52261FDAP000UX). It uses our own XNU kernel (HackiOS, xnu-8796.121.2) and a bare-metal AArch64 loader that starts from the Pixel bootloader. The device identity is iPhone10,3 / D22AP.

## Where things are
- `Firmware_extractor/` is the loader repo (github kamal894/Firmware_extractor, branch `claude/new-session-yom1ka`).
  - `caiman/probe/` holds the loader code: `main.c` lists the stages, `ufs.c`, `wifi.c`, `gpu.c`, `xnu.c` (device tree and boot-args), `watcher.c` and `dvfs.c`.
  - `caiman/probe/identity.h` is local only and never committed. It is Apple data, copied from `hakios-xnu-kernel/xnuloader/identity.h` with `char name[64]`.
  - `caiman/docs/` holds the notes. For Wi-Fi: `wifi-plan.md`, `wifi-join-analysis.md`, `wifi-tx-flowring.md`, `wifi-ios-userland.md`, `wifi-wifid-attach.md`. Others: `self-logging.md`, `usb-gadget-map.md`.
  - `caiman/tools/` holds `run_darwin.sh` (an iOS run from RAM), `mkxnuimg.py` and `ufslog.py` (reads the logs saved in boot_b).
- `hakios-xnu-kernel/` is the kernel and user space.
  - Drivers are in `drivers/`. Wi-Fi: `drivers/wifi/hakios.bcm4390.c`, `hakios.wlan.cpp`, `hakios.apple80211.c`, `hakios.sta.c`, and `drivers/net/hakios.net.c`. GPU: `drivers/gpu/`. Thermal: `drivers/power/hakios.thermal.c`.
  - Changes to Apple's XNU go in `xnu/hakios.patch`, exported with `xnu/export.sh`. Drivers are copied into `xnu-src/xnu-8796.121.2/bsd/dev/` before a build.
  - Build with `PATH=/opt/homebrew/bin:$PATH ./xnu-build.sh -j8 -k`. Exit status 2 is normal. The kernel lands in `xnu-src/.../BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837`.
  - User space: `userland/build.sh` and `userland/mkjobs.py` decide the launchd jobs. The stock iOS root is in `sysimage/rootfs`.
- `run/` holds the test scripts and images.
  - `run55.sh N` is an iOS run from RAM.
  - `run-wifi.sh N` is the same run plus fetching the log.
  - `runslota.sh` writes the daily boot_a.
  - `android-boot_a.img` is Android's original boot, used for RAM boots.
  - Redwood images (mounted at /redwood, launchd jobs in `/LaunchDaemons`): `root-same.apfs` is the base. `root-cfg.apfs` adds configd. `root-dns.apfs` adds configd and mDNSResponder.
- `backups/` holds partition backups, for example `boot_a-before-wifi-*.img`.

## Status (2026-11-21)
- Working: iOS home screen, touch, buttons, Mali GPU (Metal→Vulkan), battery, and self-logging to boot_b.
- Fixed: the GPU panic `kmem_alloc ... failed with 3`. The io_submap was raised from 8 to 256 MiB in `osfmk/arm/io_map.c`.
- Wi-Fi works end to end for the first time:
  - The loader brings the BCM4390 up and hands it to the kernel (`bcm4390_shared_pa=`, `probe/wifi_handoff.h`).
  - The kernel driver attaches en0.
  - wifid finds it once configd runs.
  - Settings joins a WPA2 network.
  - DHCP gives an IPv4 address.
- Open problem: "No Internet". Even Safari to the router's own IP fails. mDNSResponder (just added in `root-dns.apfs`) is the current suspect. The next suspects are symptomsd and nehelper (the network path), and the TX path under load.
- Daily boot: boot_a holds `loader-daily-wifi` (no time limit), the new kernel and a redwood image.
- Heat: the loader sets CPU clocks to maximum. `hakios.thermal.c` now slows the CPUs at 60, 68 and 76 °C and stops at 85 °C.

## Rules — never break
- Never touch the Samsung phone (adb serial R3GL601TEWH…).
- Wait for the battery temperature to be ≤ 36.0 °C before every device run.
- Never wipe or format storage. Wipe userdata only with the owner's explicit yes, after a backup to the SANDISK SSD.
- Back up any partition before a permanent write.
- No PMIC voltage changes. Ask before enabling any LDO.
- Wi-Fi secrets in `~/caiman-work/wifi-*.txt` and `hotspot-psk.txt`: never print, log or commit them.
- Mask the MACs and network names of neighbours and LAN devices in committed logs.
- Risky new runs only while the owner is near the phone. If it hangs: hold power for 15–20 s, then both volume keys for 3 s to reach fastboot.
- Do not run adb or fastboot, and do not flash, unless the owner asks for it right then. Claude may be running a test on the phone at the same time.
- When a question is about code, read the code and answer with exact files and lines. Say plainly when something is a guess.

## When Codex runs in the cloud (ChatGPT app / chatgpt.com/codex)
Only this GitHub repo (Firmware_extractor) is visible there. The kernel (`hakios-xnu-kernel`), the logs, the images, `run/` and the phone live only on the owner's Mac under `~/caiman-work`. In the cloud, answer from this repo's code and `caiman/docs/`. Say so when the answer needs the kernel or a log. Do not open pull requests unless the owner asks.
