# Caiman screen-off power

File references use basenames for the three XNU files named in the request. Rates are in kHz.

## 1. Blank and stop display work

Route `kFBMethodRequestPowerChange` through the framebuffer owner. Serialize transitions, and use a framebuffer presentation lock shared by `show()` and `settle()` to close the draw gate and drain any active draw/flip. Keep GPU-fence waits, swap-queue locks and the user-client notification lock outside this transition lock. Give queued swaps and deferred settles a display generation so work from before blanking cannot draw after wake. [MiniIOMobileFramebuffer.cpp:1374,1419,1603,1629,2093]

```diff
+ present(): while gated, acknowledge/drop new swaps before retaining surfaces.
+ show(): stale/gated swaps still dequeue and wake fSwapsWaiting; skip pacing,
+         composition and flip. Recheck under presentation lock before drawing.
+ presenter(): retain normal surface cleanup; cancel stale deferred settles.
+ settle(): check gate and generation under the same lock before flipBegin().
+ OFF: close gate → drain active draw/flip → mask DECON trigger → wait idle
+      → DCS 0x28 → existing 20 ms delay → apply off DVFS policy.
+ ON: restore DVFS → DCS 0x29 → restore DECON trigger → reopen gate
+     → first fresh swap composes and publishes normally.
```

For the known live DECON0 command/HW-trigger configuration only, save bits 4 and 0 at `0x19470030` and write `(old & ~0x11) | 0x10`. Read back to flush and poll `0x19470020` bit 5 with a bounded timeout (initial guess 50 ms). Restore the saved bits with RMW after 0x29 and before allowing `flipBegin()` to wait for a shadow address. A flip already published at blank may still be pending. Let HW trigger latch it on wake and require shadow confirmation before reusing either buffer, deferring the flip on timeout instead of drawing into a potentially scanned buffer. [probe/display.c:21,237; reference/google/decon_reg.c:2124,2183; MiniIOMobileFramebuffer.cpp:1557,1581]

Make `panelPower()` report status. Keep packet-go validation and DSIM short writes at `0x19440058` (`0x2805` / `0x2905`), followed by header FIFO empty at `+0x68[10]`. Bound the diagnostic RX drain and use DCS 0x0A bit 2 for confirmation when available. Commit state only after success. Before sending 0x28 an idle failure can restore the trigger and reopen drawing. After an ambiguous DCS write keep the display gated at normal rates and allow a retry. [MiniIOMobileFramebuffer.cpp:306,320,335,353]

Keep `vsyncFired()/vsync()/armVSync()` running at the current `refreshHz()` for registered clients, with advancing timestamps and counter. Keep notification registration, SwapEnd success and SwapWait success semantics. The timer is already software-driven. Keeping its cadence avoids changing backboardd's pacing contract or relying on TE while blank. [MiniIOMobileFramebuffer.cpp:1948,1955,1973,2074,2102]

## 2. Use one ACPM owner

Extend `zuma_set_level()` into a per-domain apply helper. Have `zuma_tick()/zuma_look()` and a synchronous screen-policy setter share one serialized ACPM path, including TMU channel 9, since `zuma_acpm()` owns queue indices and a shared sequence. Enable the setter only after Zuma ACPM initialization (`thermal=2`). Keep thermal polling, hysteresis and emergency stop active. [hakios.thermal.c:276,298,366,381,435,446,487]

| Domain | Off target | Wake |
|---|---:|---|
| CPUCL0/1/2 (2/3/4) | 1098000 / 1065000 / 1164000 (existing slow tier) | Current thermal tier, cool default 1696000 / 2130000 / 2499000 |
| DISP (14) | 400000 | Saved, normally 533000 |
| INT (1) | 356000 | Saved, normally 664000 |
| MIF (0) | 546000 (guess for first test, cited Android idle rate) | Saved, normally 2028000 |
| G3D (7) | 580000, already at the lowest documented step | Saved, normally 580000 |

421000 MIF, 356000 INT and 400000 DISP were observed at loader entry. Their stability under live XNU workloads is unverified. A lower G3D rate has no evidence here. Only add one after identifying a supported lower OPP for this board and validating it with the Mali path. [probe/dvfs.c:160; docs/probe-log-dvfs5.txt:7,36; hakios.thermal.c:276]

```diff
+ get(d):       zuma_acpm(1, {d, 0, 1, 0}); check reply[3], read reply[1].
+ set(d, khz):  zuma_acpm(1, {d, khz, 0, 0}); check reply[3], verify with get(d).
+ zuma_look(): update thermal tier separately from applied CPU state;
+              effective CPU tier = max(thermal tier, off ? 3 : 0).
+ screenOff(): snapshot non-CPU rates before first write; apply enabled domains
+              individually, DISP → INT → MIF for the combined trial.
+ screenOn():  restore MIF → INT → DISP before DSIM/DECON wake; recompute CPU tier.
```

Check each domain before committing it. On a partial failure, reconcile the attempted domain via GET (timeout may mean the write landed) and restore modified domains in reverse order. Keep drawing gated on an unverified wake restore and retry via the same serialized path. Leave DSU/BCI alone. [probe/dvfs.c:8,134,173,208; hakios.thermal.c:321,338,409]

## 3. Stage tests independently

1. **Display only, stock clocks.** First test the software gate with DCS alone. Then enable HW trigger masking. Check draw/flip counts stop, DECON frame count stays flat after drain, software vsync timestamps/counter advance, and swaps do not back up. Repeat lock, idle blank and rapid wake during composition with flip/filter enabled. Check first fresh frame and shadow address on wake. DECON MMIO itself can hang when unpowered, so keep the DSIM/DECON/DPP clocks and power domains live. A polling timeout cannot rescue a stalled MMIO read. [probe/display.c:330; MiniIOMobileFramebuffer.cpp:1390,1564,1992]
2. **One rate at a time.** Try CPU first, then DISP, INT and MIF separately with the display gated. Log request/reply and GET on entry/exit. Exercise GPU and background I/O off-screen, verify wake and thermal throttling, then combine. Low INT/MIF can starve other DMA clients, including Mali, even with DECON idle. Avoid raw clock, PMIC or power-domain writes. [probe/dvfs.c:8,160; hakios.thermal.c:409]
3. **G3D separately, if a lower OPP is confirmed.** Use `mali_submit_on()` as the common admission point, serialize new submissions against the change, and drain each live stream's last fence through the completion path (`mali_done()` also services CSF events). Release admission after each verified rate change. A blank panel or a single idle event does not prove GPU idle. Abort on a drain timeout and keep 580000. Test all streams, fence latency, faults and rapid wake before combining with fabric reductions. [hakios.adreno.c:4158,4209,4234,4660]
