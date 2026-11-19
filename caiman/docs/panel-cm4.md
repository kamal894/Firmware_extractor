# Pixel 9 Pro CM4 panel

The byte sequences below come from disassembly and relocation data in the local `~/caiman-work/vkb/rd/lib/modules/{panel-gs-cm4,gs-panel}.ko` (CM4 Build ID `e9df9d8dfe0283852ea18e9c507556204e42711b`). Gitiles lists the CM4 source in the separate Caimito panel repository, but the source blob was unavailable during this check. Function names are from the module symbols. Each semicolon in a byte sequence separates DCS packets. [Google Caimito panel tree](https://android.googlesource.com/kernel/google-modules/display/panels/caimito/+/refs/heads/android-gs-comet-6.1-android15-qpr1)

## Display off and on

- **Disable.** `panel-gs-cm4.c:cm4_disable` returns early for LP mode. Otherwise it calls `gs_panel.c:gs_panel_disable` (normal-feature cleanup, with an empty common off-command slot for CM4), then programs manual/peak refresh via `cm4_enforce_manual_and_peak` if there is a current mode. The normal-mode packets are `F0 5A 5A; BD 21; 60 00; F7 0F; F0 A5 A5`. The helper uses `60 18` for the alternate normal refresh feature bit, or `B0 00 01 60; 60 00` in LP context. Then `28`, wait **20,000–20,010 µs**. For the full OFF state only, send `10`, wait **100,000–100,010 µs**. These packets use the F0 key for vendor programming. There is no FC key in this sequence. `gs_panel.c:gs_panel_unprepare` separately calls `gs_panel_set_power_helper(..., false)` to power down.
- **Cold enable after full OFF.** `panel-gs-cm4.c:cm4_enable` calls `gs_panel_reset_helper`, sends `9D 01` and the mode’s DSC PPS via `gs_dcs_write_dsc_config`, sends `11`, waits **120,000–120,010 µs**, sends the init table below, then calls `cm4_te2_setting`. Next comes `F0 5A 5A; C3 0C; B0 00 01 F2; F2 81; F0 A5 A5` at 1280 pixels wide, or `C3 0D` and `F2 01` at 960 wide. For normal mode it calls `cm4_update_refresh_ctrl_feat`, `cm4_set_panel_feat`, `cm4_write_display_mode`, and `cm4_change_frequency`, then sends `29`. LP enable calls `cm4_set_lp_mode` before `29`.
- **Powered blank/unblank.** The CM4 driver has a warm blank path. Disable stops after `28` and the 20 ms wait. Enable still sends DSC and reapplies the mode programming, but skips reset, `11`, the 120 ms wait and the init table, and sends `29` when coming from blank. **Inference for an already configured loader:** `28` → wait 20 ms → `29` is the minimal candidate if rails, DSC, mode and panel configuration are retained. The driver’s warm path is evidence for keeping power and reset intact. A reset-free wake after sending `10` is unverified.

`panel-gs-cm4.c:cm4_init_cmds`, in order, with zero per-entry delay. Expand the repeated eight-byte group within the same C5 packet. TE enable is the one-byte `35` entry.

```text
35
2A 00 00 04 FF
2B 00 00 0B 27
F0 5A 5A
B0 00 36 C5
C5 10 10 50 05 [4D 31 40 00 40 00 40 00 × 4]
B0 00 1D 63
63 02 18
B0 00 13 B1
B1 80
F7 0F
F0 A5 A5
```

## Brightness

`panel-gs-cm4.c:cm4_panel_config` selects `cm4_btr_configs` through `gs_panel.c:gs_panel_update_brightness_desc`. The first matching revision mask wins and the last row is the fallback. The entries give endpoint pairs, with interior luminance dependent on the brightness curve.

| Revision mask | Normal DBV | Normal nits | HBM DBV | HBM nits |
|---|---:|---:|---:|---:|
| `0x80000010` | 176–3271 | 2–1250 | 3272–4095 | 1250–2489 |
| `0x000004B9` | 184–3427 | 2–1250 | 3428–4095 | 1250–2489 |
| `0x00000002` / fallback | 186–3406 | 2–1200 | 3407–4095 | 1200–2489 |

`panel-gs-cm4.c:cm4_set_brightness` writes nonzero normal-mode DBV with `51 MSB LSB`. Zero takes the `22` (all pixels off) path, and the next nonzero level first sends `13` (normal mode). Ordinary changes within an already configured normal mode use `51`. HBM is an explicit mode change in `cm4_set_hbm_mode`, which also applies vendor feature programming through `cm4_set_panel_feat`. `cm4_write_display_mode` writes `53 20` normally or `53 E0` in HBM, OR-ing in `08` for dimming. `55` is ACL, with separate policy in `cm4_acl_modes_manager`. Its DBV threshold of **3726** (`0x0E8E`) is distinct from the HBM boundary.

## AOD and host timing

- **AOD.** `panel-gs-cm4.c:cm4_set_lp_mode` enforces manual/peak refresh and waits for VSYNC completion, then sends `F0 5A 5A; 53 24; B9 51; B0 00 E8 BD; BD 00; B0 00 10 BD; BD 22; B0 00 82 BD; BD 22 22 22 22; F7 0F; F0 A5 A5`. It then applies LP refresh/DBI settings and the night/low/high brightness bin (`51 00 B0`, `51 01 6D`, or `51 02 F6`). `cm4_set_nolp_mode` waits for VSYNC completion, enforces manual/peak refresh, waits again and sleeps 5.0–5.1 ms, then sends `F0 5A 5A; 53 20; F0 A5 A5` and reapplies normal-mode features and refresh. These LP entry/exit paths use vendor refresh programming and `53` rather than DCS idle commands `39`/`38`.
- **DECON/DSIM.** The local `exynos_drm_decon.c:decon_wait_for_flip_done` masks the command-mode trigger after the flip unless `keep_unmask` is set. `_decon_wait_for_framedone` drains pending frames before controller shutdown. In `~/caiman-work/ref/disp/exynos_drm_dsim.c`, `dsim_write_data_locked` and `dsim_cmd_flush_locked` show command queue/flush handling. **Inference for the powered loader path:** stop new frame submissions, let any in-flight frame finish, mask DECON’s HW trigger, and send the panel commands through the working DSIM command path without depending on a new TE. After `29`, rearm the frame path, using a fresh TE if HW-triggered. The simple `28`/`29` paths contain no unconditional extra frame wait beyond the 20 ms off delay. `cm4_enable` does have a conditional VSYNC wait before DSC programming for a refresh transition.

## Screen off and on from iOS (2026-11-18, runs ws60-ws62)

- **What iOS asks.** backboardd calls IOMobileFramebuffer selector 12 (`kFBMethodRequestPowerChange`) with 0 for
  off and 1 for on: once while SpringBoard starts (off, then on 1.7 s later), and on the power key (run ws60,
  `fb_log_all=1 press_power=200`).
- **What the kernel does now.** `MiniIOMobileFramebuffer.cpp` `panelPower()`: with boot-arg `panel_dsim=<pa>` (the
  loader adds `panel_dsim=0x19440000`), 0 sends DCS `28` (display off) and waits 20 ms, and 1 sends `29` (display
  on). This is the warm path above: no reset and no `10`/`11`. Packet-go must be off, as the bootloader leaves it.
  DCS `0a` is read back for the log.
- **Result (ws61).** Off reads back power mode 0x99 (bit 2, display on, is clear) in 21 ms. On reads back 0x9d in
  15 us. Each off and on was seen at boot and on both key presses.
- **Open.** The first press is answered off, then on 170 ms later; the second press leaves the screen off. This is
  iOS's own choice (no panel error); not looked into yet.
- **Not done yet.** Frames are still sent while the panel is off, the CPU and GPU stay at their clocks, and there is
  no system suspend.
