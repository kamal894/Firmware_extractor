# Metal on the Mali-G715: the plan

Where we start (2026-11-17, docs/gpu-plan.md steps 1-8): the loader boots the Mali's CSF firmware; a
compute shader and a triangle run from our own command streams; inside XNU, `mali=1` backs HakiosGPU's
kernel API (addresses, AS1 mapping, CALL submission, fences) with the Mali, and a self-test submits a
command buffer through it. What iOS still lacks is a user-space driver that turns Metal into Mali work.

## What HackiOS has for the Adreno (docs/reference/metal.md in the HackiOS tree)

| Path | Route | Shaders | Speed there |
|---|---|---|---|
| hakios-metal | Metal -> HakiosMetalCA -> hakios-metal (Adreno PM4) -> HakiosGPU | AIR -> NIR -> ir3 (Mesa), in process | 100-104 fps swipes |
| metal2vulkan | Metal -> AdrenoMetal's frontend -> metal2vulkan -> **Turnip** (Mesa Vulkan) -> hakios-drm -> HakiosGPU | AIR -> Vulkan pipelines, Turnip compiles | 50-57 fps |

Both are Adreno-specific only below the line: hakios-metal's PM4/a6xx encoders and its ir3 back end;
metal2vulkan's Turnip and the msm requests of hakios-drm. Everything above (QuartzCore's metallib,
HakiosMetalCA's plumbing, the metal2vulkan frontend) is GPU-neutral.

## The choice

**A. metal2vulkan over PanVK (recommended first).** Replace Turnip with Mesa's PanVK (Vulkan for Mali,
CSF, v10-v13; the code our triangle was built from) and hakios-drm's msm requests with Panthor's. The
shader compiler (Valhall), descriptors, tiler handling, render passes and blending come with PanVK.
Work: build PanVK for iOS with the existing cross toolchain (tools/mk-turnip.sh is the model); a
`hakios-panthor` shim in place of libdrm (Panthor's ioctls -> HakiosGPU requests); and the kernel side
of those requests: VM bind, BO create/mmap, groups of several streams with their rings, submission of
ring-buffer slices, sync objects, tiler heaps. Expected speed: below hakios-metal's, but the shortest
road to a GPU-drawn window server.

**B. hakios-metal for Mali.** Keep hakios-metal's Metal object model (command buffers, encoders,
pipelines) and replace its PM4/a6xx layer with CSF command streams and Valhall descriptors (what
probe/gpu.c does by hand today), and ir3 with Mesa's Valhall compiler (bifrost/valhall in
src/panfrost/compiler, which compiles NIR the same way ir3 does). Faster in the end, much more of our
own code. Worth doing after A works, piece by piece.

## Order of work for A

1. Inventory of what PanVK asks of the kernel (Codex: docs/panvk-uapi.md, in progress).
2. Kernel: several streams per group (PanVK uses one per subqueue: vertex-tiler, fragment, compute), each
   with its own ring and doorbell; submission of a CS buffer slice per stream; in-memory sync objects;
   tiler heap contexts (chunks given in advance; out-of-memory handling later); per-client AS (one
   shared AS1 at first, as HakiosGPU does for the Adreno).
3. `hakios-panthor`: the libdrm/kmod stand-in, by the model of tools/mesa/hakios-drm.
4. Build PanVK for iOS (Mesa's meson with the iOS cross file of mk-turnip.sh, `-Dvulkan-drivers=panfrost`).
5. A Vulkan test program on the phone: vkCreateDevice, then a compute dispatch, then a triangle.
6. metal2vulkan over it, with `metal=1 metal_m2v=1`: the window server through Metal on the Mali.
7. Then speed: interrupts instead of polling, the GPU clock by load, B where it pays.

## What is known to be hard

- PanVK expects Linux's Panthor semantics (VM_BIND asynchronous ops, syncobjs, group priorities); the shim
  can serialise much of it, as hakios-drm does for Turnip.
- The tiler heap: PanVK and the firmware grow it on out-of-memory events through the kernel; a big
  pre-allocated heap avoids that for a start (our triangle did with 1 MiB and no HEAP_SET).
- Protected memory, power management and GPU reset are out of scope at first.
