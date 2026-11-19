# The flicker (2026-11-19): frames shown before the Mali had drawn them

Seen by the owner everywhere (Settings too), not tied to touch. One picture a second (PROBE_SHOTS) showed nothing.
`PROBE_SHOTS_FAST=1` (watcher.c: two bursts of 25 pictures 40 ms apart, at second N during the touch_demo swipes
and N + 50 at rest) caught it: frames with the lower half black, torn rows, half-drawn widgets
(shots/flicker-before.png).

Cause: MiniIOMobileFramebuffer waits, before copying a swap's surface to the panel, for `MiniIOSurfaceDrawn()`,
which waits for the surface's `drawn` fence. Only the msm/freedreno submit path names surfaces and sets it; the
Mali path (HakiosGPU 0x91, `hakios_mali_submit`) names none, so with mali=1 the wait passed at once and frames
were copied while the GPU was still writing their tiles: tiles not yet written came out black.

Fix (kernel, hakios/caiman-kernel.patch):
- `MiniIOSurfaceGPULast()` / `MiniIOSurfaceGPUDone(fence, ms)` (hakios.gpu.cpp): the GPU's newest submission,
  and a wait for everything up to it (Mali: all streams);
- SwapEnd records the newest submission for its slot; the presenter waits for it before drawing (counted as
  fStatGPULate when it gives up after SWAP_GPU_MS);
- `fb_safe=1` (default): a swap's slot is let go only after its picture is in the panel's memory, one swap
  waiting at most (CoreAnimation's two surfaces could otherwise be drawn into mid-copy). fb_safe=0: as before.

Result (shots/flicker-after.png): 50 of 50 clean pictures, swipes at 36-41 frames a second as before.
