# GPU: Mali-G715 on caiman

Android: `Mali-G715 7 cores r1p3 0x0B080A02` (sysfs gpuinfo); CSF firmware files copied to
`~/caiman-work/gpu/` (`mali_csffw-*.bin`; the driver logs "Loading Mali firmware 0x2080000 ...
Firmware boot completed"). DT `mali@1F000000` (0x480000), interrupts JOB 0x15b, MMU 0x15c, GPU 0x15a,
power domains pd-g3d@15461A80 ("top", switched through an SMC) and pd-embedded_g3d@15461C80 ("cores"),
S2MPU in front (`s2mpus`).

## Step 1: read only (2026-11-17, PROBE_GPU=1, docs/probe-log-gpu1.txt)

- Both power domains are **on** when the bootloader hands over (+0 = 1, +4 status = 1).
- GPU_ID 0xb8a21030: arch 11.8.10, product 2, r1p3 (the same GPU Android reports).
- SHADER_PRESENT 0x01110055 (7 cores), TILER 1, L2 1, AS_PRESENT 0xff, CSF_ID 0x04100412,
  MMU_FEATURES 0x2830 (VA 48, PA 40 bits).
- L2, shaders and the MCU are off (READY 0, MCU_STATUS 0): nothing has started it.

## Step 2: reset and power (PROBE_GPU=2, run gpu2)

Soft reset (GPU_CMD 0x101) done in 45 us; L2 on in 100 us, tiler 15 us, all 7 shader cores in 390 us.
The GPU clock (ACPM DVFS domain 7) is at 150 MHz, its lowest step (Android's table: 150-940 MHz).

## Step 3: the firmware boots (PROBE_GPU=3, run gpu6, docs/probe-log-gpu6.txt)

`mali_csffw-r54p3.bin` (the one Android loads, git_sha 690855d0) is packed after the probe
(`tools/pack_aoc_payload.py ... GPUBLOB`). Its 7 non-protected sections are copied to DRAM at
0xf0000000+ and mapped in AS0; the PROT section is left out (kbase too boots without a protected
allocator). What it took, after runs gpu3-gpu5 faulted on the first page-table read (a GPU bus fault, not
the S2MPU, which reads disabled, and not the address: 0xd0000000 failed the same), from Codex's reading of
`mali_kbase.ko` (docs/mali-boot.md):

- table entries `| 0x403` (AF set), leaves type 3 + AF, AP 01/11, SH 0, XN bit 54 only, MEMATTR index 0
  (5 for the shared section);
- TRANSCFG 0x42000006, then TRANSTAB, then MEMATTR 0x00084c8d888d8f88;
- ASN_HASH 0x36db6d/0x5b6db6/0 at 0x2c0.. and L2_CONFIG bit 24 before the L2 is powered (reads back
  0x01140000, as Android's), COHERENCY_ENABLE 31.

Then MCU_CONTROL = 2: **JOB_IRQ bit 31 after 3.4 ms**, global interface version 0x02080000, input VA
0x04002000, output VA 0x04006000, 8 CSGs (stride 0xa0). No faults.

## Step 4: the first job (PROBE_GPU=4, run gpu7, docs/probe-log-gpu7.txt)

From docs/mali-csf-submit.md (Codex, from mali_kbase.ko and Mesa's arch 11 CS encodings). After boot:
GLB_ALLOC_EN = 0x1110055 (request bit 2, bell 0) acknowledged; AS1 with its own tables (ring page at VA
0x100000, target page at 0x200000, both MEMATTR slot 5 = non-cacheable); CS0 of CSG0 (ring, queue pages
and a 26368-byte suspend buffer in AS0, doorbell 1) and CSG0 (all cores, AS1) started: CSG/CS ack state 1.
The ring: MOVE48 r0 = 0x200000, MOVE32 r2 = 0x89abcdef, SET_STATE SB_SEL_OTHER 0, STORE_MULTIPLE r2 ->
[r0], WAIT slot 0; INSERT = 64, bells 1 and 0.

**The target read 0x89abcdef 191 us later**: the GPU ran our first command stream. Queue extract 64,
no CS fault. Open: MMU_IRQ_RAWSTAT shows 0x20000 (bit 17, AS1 bus-fault bit) from the AS1 set-up; it
did not stop the job.

## Step 5: the first shader (PROBE_GPU=5): WORKS since run gpu20

**Run gpu20: 64 of 64 invocations wrote their global id (buf 0, 1, 2 .. 0x3f) 64 us after the stream
started; EP_CURRENT 7, no faults.** The missing piece, found from Codex's note that CS0 posted no
resource request: the stream must ask for compute endpoints with REQ_RESOURCE (opcode 34, bit 0 compute;
Mesa v11.xml, panvk's `cs_req_res` at queue init) before RUN_COMPUTE. The history below is how it went.

### Runs gpu8-gpu19

`probe/gpu_id.asm` (4 instructions: id = r60, store at 0x200000 + id*4) assembled with Mesa's Valhall
assembler (`src/panfrost/compiler/bifrost/valhall/asm.py`, Mesa main 2026-11): words in gpu.c. SPD (type 8,
compute, preload r60 = bit 12, binary at 0x300000, executable), TSD (no TLS/WLS), CS registers per Mesa
v11.xml COMPUTE SR (SRT r0, FAU r8, SPD r16, TSD r24, WG_SIZE r33 = 63, JOB_SIZE r37-39 = 1), RUN_COMPUTE
(opcode 4, task increment 1), WAIT all.

- gpu8: only the stream's bell rung: the stream did not fetch (extract stayed 64).
- gpu9-12: with the group and global bells it fetches up to RUN_COMPUTE (extract 160) and stops there:
  GPU_STATUS busy, no CS fault or fatal, no MMU fault, shader cores ready, buffer untouched. CSG0 output
  +0x10/+0x14 (endpoints) read 0, also after toggling CSG_REQ bit 4 (acked) with the endpoint word in
  +0x34 and +0x38. Next: find in mali_kbase.ko what r54p3 does for a group to get shader endpoints
  (CSG status update, GLB shader power requests, the iterator/endpoint config layout for interface 2.8).

Runs gpu13-gpu16 (after docs/mali-endpoints.md from Codex): shader and descriptor pages mapped before AS1
goes live (no AS1 fault either way); tiler/shader power left to the firmware (gpu14: then nobody powers
them, SHADER_READY 0) and powered by us after the MCU boots (gpu15). With a CSG status update (CSG_REQ
bit 5, acked) the snapshot is: CS0 command pointer 0x1000a0 = the RUN_COMPUTE, CSG0 output +0x10 0
(current endpoints, Panthor's layout) and +0x14 0x00070707 (requested). So the stream waits at
RUN_COMPUTE for shader endpoints the firmware does not grant. Open: what grants them in r54p3 (GLB
requests not made yet: progress/poweroff/idle timers, shader-core host/firmware handshake).

Runs gpu17-18, with the Pixel kbase GPL source (android.googlesource.com kernel/google-modules/gpu,
android-gs-caimito-6.1-android16, copied to ~/caiman-work/kbase-src/tree): gpu17 kbase's hctl order
(timers and an empty ALLOC_EN, host powers cores, ALLOC_EN again); gpu18 the firmware-managed order
(global_init: ack mask, ALLOC_EN, power-off timer 19, progress timer, one request; host leaves cores off).
Both: all requests acked, the same stall at RUN_COMPUTE with no endpoints; in gpu18 nobody powers the
cores. Our CSG programming matches program_csg_slot() field for field (EP_REQ at +0x34 for arch < 14.8.4).

Run gpu19, everything in docs/mali-endpoints-3.md (Codex, from the GPL source) applied: global_init with
the unshifted power-off timer 0x4ccc (+0x88 = 1), progress 0x271000, CSG eviction timer 4 s (+0x90/+0x94,
request bit 16), kbase's ACK IRQ mask 0x4c810f1e (GLB_ACK 0x0001000e: all acked); CS START with the idle
options (CS_REQ 0x1501) and IRQ mask; CSG priority 15 and IRQ mask; STORE and compute queued in one batch
before START; JOB IRQs cleared and CSG IRQ requests acknowledged while polling. The STORE runs; the stream
again stops at RUN_COMPUTE (0x1000a0) with EP_CURRENT 0, and the firmware never powers a shader core
(SHADER_READY 0 throughout).

Run gpu20: the same with `REQ_RESOURCE compute` first in the compute block: done in 64 us.
Run gpu21: with PROBE_DVFS=2 the GPU clock (ACPM DVFS domain 7) goes 150 -> 580 MHz (read back), and the
same dispatch takes 34 us; firmware boot 1.4 ms instead of 3.5-4.6.

## Step 6: the first triangle (PROBE_GPU=6): WORKS, run gpu24

Built by hand from Mesa's panvk CSF draw path (docs/mali-triangle.md has every value and its source line),
shaders probe/gpu_tri_pos.asm and gpu_tri_frag.asm assembled with Mesa's Valhall assembler. In one
command stream after step 5's compute: REQ_RESOURCE fragment|tiler|IDVS, the IDVS staging registers,
RUN_IDVS (malloc), FINISH_TILING, WAIT, then FBD_POINTER, bounding box, RUN_FRAGMENT, WAIT. One 1 MiB
tiler heap chunk given in advance and no HEAP_SET was enough for one triangle.

- gpu22: drawn on the first try (centre red, corner green), counted before the last tiles were written.
- gpu23: tiles with no primitive were never written back (left 0xdeadbeef).
- gpu24: with the render target's clean-tile-write bit: **1152 red + 2944 green = all 4096 pixels**,
  1152 being exactly the triangle's area ((48 x 48) / 2), 3.9 ms, no faults.

```
gpu: compute: 64 of 64 invocations wrote their id after 34 us: THE SHADER CORES RAN OUR SHADER; buf 00000000 00000001 00000002 .. 0000003f; extract 512, CS0 status 0x00000000 blocked 0x00000000 fault 0x00000000 fatal 0x00000000 (info 0x0000000000000000)
gpu: triangle: centre 0xff0000ff corner 0xff00ff00 after 3925 us; 1152 red, 2944 green, 0 other pixels: THE GPU DREW A TRIANGLE; extract 512
gpu:   row  0:................................
gpu:   row  4:................................
gpu:   row  8:....########################....
gpu:   row 12:.....######################.....
gpu:   row 16:......####################......
gpu:   row 20:.......##################.......
gpu:   row 24:........################........
gpu:   row 28:.........##############.........
gpu:   row 32:..........############..........
gpu:   row 36:...........##########...........
gpu:   row 40:............########............
gpu:   row 44:.............######.............
gpu:   row 48:..............####..............
gpu:   row 52:...............##...............
gpu:   row 56:................................
gpu:   row 60:................................
gpu: triangle CS0 out +0x40..: 00100200 00000000 00000000 0000000e 00000000 00000000 00000000 00000000 fault 0x00000000 fatal 0x00000000
```

## Step 7: the GPU from inside XNU (run darwin-mali1)

The loader (`LOADER=1 ... PROBE_DVFS=2 PROBE_GPU=6 MALI_FW=.../mali_csffw-r54p3.bin`: the firmware is built
into the image by probe/gpu_fw.S, since the loader's load base + 1 MiB is XNU's payload) runs steps 1-6
and leaves the firmware running, its memory at 0xf0000000+ outside XNU's; /chosen/mali-handoff gives the
shared interface, ring, queue and AS1 target pages and the ring's INSERT. The kernel (`mali=1`,
drivers/gpu/hakios.adreno.c `mali_handoff()`, in hakios/caiman-kernel.patch) maps them, appends a STORE
and rings the bells:

```
mali: GPU_ID 0xb8a21030, MCU_STATUS 1, SHADER_PRESENT 0x1110055; firmware interface 0x2080000; ring insert 512
mali: from XNU: target 0x105c0de5: THE KERNEL DROVE THE GPU; ...
```

iOS came up as usual afterwards.

## Step 8: a Mali back end for HakiosGPU (run darwin-mali2)

HakiosGPU (drivers/gpu/hakios.gpu.cpp, the user client Metal and Mesa talk to) does not touch the GPU; it calls
hakios_gpu_address/map/mapped/unmap/submit/done/last. With `mali=1` those now go to Mali code in
drivers/gpu/hakios.adreno.c: AS1 addresses from 4 GiB up; AS1 page tables (the loader's level 0, more tables
from the free DRAM after the loader's allocations, handed over in /chosen/mali-handoff) with kbase's 4 KiB
entries, then FLUSH_MEM; submission = per command buffer MOVE48/MOVE32/CALL (opcode 32) into the stream's
ring, then a fence STORE at AS1 0x200c00, INSERT and the doorbells; done = the fence word. The kernel then
starts the HakiosGPU service. A self-test does what a client does (a kernel page with a command buffer,
mapped, submitted, waited for):

```
mali: self test: a command buffer in kernel memory (phys 0x8fc4c8000) at GPU address 0x100000000, fence 1 done;
      it wrote 0xc0ffee11: SUBMISSION THROUGH THE HAKIOSGPU API WORKS
```

## Step 9: three streams per group (PROBE_GPU=7, run gpu25)

PanVK's groups have three command streams (docs/panvk-uapi.md). Group 0 now starts with streams 0-2: stream 0
keeps its 4 KiB ring at AS1 0x100000; streams 1 and 2 get 64 KiB rings at 0x110000 / 0x120000, queue pages at
Q_VA + 0x2000 s, doorbells 2 and 3, CS_CONFIG priority 1, CS_REQ START with the idle bits; all three set up
before CSG START, the CSG's stream doorbell bits 0-2 toggled together. After steps 4-6 a STORE through each
of streams 1 and 2 (using r92-r95, the kernel's registers under PanVK's convention):

```
gpu: stream 1: CS ack 0x00000001 (state 1), extract 0, wrote 0x11110001: OK
gpu: stream 2: CS ack 0x00000001 (state 1), extract 0, wrote 0x22220002: OK
```

/chosen/mali-handoff grows to 20 words: the 8 before, then for each stream its ring PA, ring size, queue PA
and INSERT.

## Step 10: the kernel on three streams (run darwin-mali3)

The kernel's Mali back end takes over all three streams from the handoff and submits on any of them, the
way Panthor does it: a FLUSH_CACHE2 (L2 and LSC clean+invalidate, other invalidate) first, then a CALL per
command buffer, then the stream's fence (AS1 0x200c00 + 4 s), all with the kernel's registers r92-r95;
done() checks every stream's fence. Self test, one command buffer submitted on each stream from XNU:

```
mali: self test, stream 0: ... fence 1 done; it wrote 0xc0ffee10: OK
mali: self test, stream 1: ... fence 2 done; it wrote 0xc0ffee11: OK
mali: self test, stream 2: ... fence 3 done; it wrote 0xc0ffee12: OK
```

## Step 11: a tiler heap in kbase's format with HEAP_SET (PROBE_GPU=8, run gpu26)

The triangle's 1 MiB heap as four 256 KiB chunks, each with a 64-byte header whose first u64 points at the
next ((address & ~0xfff) | size >> 12), a 32-byte heap context pointing at the first, and HEAP_SET (opcode
48) with the context before the draw, as PanVK does at queue start; the Tiler Heap descriptor names the first
chunk. The triangle comes out the same (1152 red, 2944 green), no faults: the firmware takes the format.

## Step 12: tiler heaps from the kernel (run darwin-mali4)

`mali_tiler_heap_create(chunk_size, count, &first)` in the kernel (what Panthor's TILER_HEAP_CREATE does for
PanVK): wired kernel chunks mapped at consecutive AS1 addresses, linked through their headers, and a heap
context; returns the context's and the first chunk's GPU addresses. Self test with PanVK's own request (5 x 2
MiB), then HEAP_SET with it from a command buffer on stream 0:

```
mali: self test, tiler heap: context at GPU 0x100010000, first chunk 0x100020000 (5 x 2 MiB); HEAP_SET then a store: 0x4ea90001: OK
```

## Step 13: user space drives the Mali through HakiosGPU (run darwin-mali7)

Three requests in HakiosGPU for a Panthor stand-in: 0x90 query (GPU and CSF properties: 8 CSG slots, 3
streams, 96 registers of which 4 the kernel's, 16 scoreboards), 0x91 submit one user command stream on one of
the three streams (returns a fence for msm WAIT_FENCE), 0x92 tiler heap. Buffers, their GPU addresses and
their mapping into the process are HakiosGPU's msm requests as they were (GEM_NEW, GEM_INFO IOVA, client
memory), now backed by AS1. userland/src/mali_test.c (rc.jobs.malitest), an iOS program:

```
mali_test: query 0: GPU_ID 0xb8a21030, shader cores 0x1110055, tiler 0x1, L2 0x1; CSF 0x4100412; 8 CSG slots, 3 streams, ...
mali_test: buffer 1: 0, GPU address 0x100a20000, mapped here at 0x101024000 (65536 bytes)
mali_test: stream 0: submit and wait 0, fence 5; the word is 0x5e70000: USER SPACE DROVE THE GPU
mali_test: stream 1: submit and wait 0, fence 6; the word is 0x5e70001: USER SPACE DROVE THE GPU
mali_test: stream 2: submit and wait 0, fence 7; the word is 0x5e70002: USER SPACE DROVE THE GPU
mali_test: tiler heap 0: context 0x100a30000, first chunk 0x100a40000
```

**Open: the clocks disagree.** msm WAIT_FENCE's timeout is an absolute time that the kernel reads as
mach_continuous_time in ns (82.7 s at the test), but user space's CLOCK_MONOTONIC said 0.62 s (timebase
15625/384 is right; libc's CLOCK_MONOTONIC is wall time minus kern.boottime, so boottime is set wrong on
caiman, probably when the wall clock is first set). A deadline of now + 1 s is "over before it began"
(darwin-mali6). hakios-drm, and so PanVK through the shim, computes deadlines from CLOCK_MONOTONIC: fix
boottime (or have the kernel read the same clock) before PanVK.

**Fixed (run darwin-mali8):** HakiosGPU reads msm times by user space's CLOCK_MONOTONIC (calendar time less
boot time, `monotonicNow()` in hakios.gpu.cpp) instead of mach_continuous_time, which on the Pixel has run
some 80 s longer (the phone's boot loaders and ours count). A now + 1 s wait now waits: all three streams
pass with Mesa-style deadlines.

## Step 14: PanVK built for iOS; the kernel's address split and VM bind (run darwin-mali9)

**PanVK for iOS.** tools/mk-panvk.sh (kernel tree): Mesa main (da2d473; 25.0.7 has no v11 PanVK) with
tools/mesa/panvk-darwin.patch, two builds: native host tools (mesa_clc, vtn_bindgen2, panfrost_compile, from
Homebrew's LLVM 23, SPIRV-LLVM-Translator, SPIRV-Tools, libclc) and the iOS one with xnu-clang and the
iPhoneOS 16.5 SDK. Out: libvulkan_panfrost.dylib, 13.8 MB, arm64. The patch: Panfrost's job-manager kmod
left out on Darwin (eventfd, timerfd); vk_image's DRM modifier field and vk_drm_syncobj.c on Darwin;
sys/sysmacros.h; `-undefined dynamic_lookup` (weak entrypoints, as turnip-darwin.patch). It wants 15 libdrm
functions from hakios-panthor (drmIoctl, drmGetVersion, the syncobj ones, ...).

**Address split, as Panthor's:** the lower half of the 48-bit GPU space is the user's (PanVK takes 32 MiB up),
the kernel's own allocations moved to 0x800000000000 (the loader's first 32 MiB stay). Page tables are found by
their 16 KiB chunk of the loader's pool (the 512-entry list was too slow and too small for PanVK).

**VM bind (request 0x93):** a buffer's pages at a user address, or nothing there. mali_test binds its buffer
at 0x40000000, runs a command stream from there that stores through that address, then unbinds:

```
mali: self test, stream 0: ... at GPU address 0x800000000000, fence 1 done; it wrote 0xc0ffee10: OK
mali_test: bound at 0x40000000: bind 0, submit and wait 0; the word is 0xb19d0001: THE GPU RAN FROM A USER-CHOSEN ADDRESS
mali_test: unbound: 0
```

## Step 15: PanVK runs on iOS: the first Vulkan work on the Mali (run darwin-panvk10)

userland/src/vk_test.c (Turnip's round, the driver chosen by path) with PanVK and hakios-panthor:
vkCreateInstance, the Mali as a physical device, vkCreateDevice (PanVK's own BOs bound at the top of the
user space, its queue's group and tiler heap made, its first submissions waited for), then vkCmdFillBuffer
of 4 MB, read back and checked:

```
vk_test: device "Mali-G715 MC7", Vulkan 1.4.368, driver 0x6863063, vendor 0x13b5 device 0xb8a21030
vk_test: the GPU filled 1048576 of 1048576 words with 0x600dc0de in 1.51 ms
vk_test: 100 fills of 4 MB, each submitted and waited for: 0.683 ms on average, 0.557 at best
vk_test: PASS
```

What it took, after step 14 (panvk-darwin.patch and hakios-panthor): the Mach-O LC_UUID in place of the ELF
build-id; PAN_I_WANT_A_BROKEN_VULKAN_DRIVER=1 (Mesa calls v11 "not well-tested"); the DRM sync provider built
on iOS (without it pan_kmod_dev_init() gives up silently: found by Codex); CTR_EL0 from hw.cachelinesize
(user space may not read it on Darwin: SIGILL); drmGetCap saying syncobj timelines are there; VM binds of
part of a page rounded up to the page, as Panthor does.

## Step 16: Vulkan compute and a Vulkan triangle through PanVK (run darwin-vktri1)

userland/src/vk_tri.c (shaders: GLSL in src/vk_tri/, SPIR-V by glslangValidator on the Mac): a compute shader
writing i * 3 + 7 into 4 MB, then a render pass clearing 256 x 256 RGBA8 to green and drawing a red triangle from
the vertex index, copied out and counted. PanVK's own compiler (Valhall) built both pipelines on the phone:

```
vk_tri: compute pipeline made (the shader compiled for the Mali) in 9.4 ms
vk_tri: compute: 1048576 of 1048576 words right in 1.24 ms: THE MALI RAN A VULKAN COMPUTE SHADER
vk_tri: graphics pipeline made in 2.5 ms
vk_tri: triangle: 18432 red, 47104 green, 0 other of 65536 pixels (28% red; 28% by geometry) in 1.26 ms
```

Nothing new was needed in the kernel or hakios-panthor: the vertex/tiler, fragment and compute streams, the
tiler heap and the binds all work as PanVK uses them.

## Step 17: Metal on the Mali (metal2vulkan over PanVK): m2v_test PASS (run darwin-m2v9)

tools/mk-m2v.sh builds UniXNU's AdrenoMetal frontend and steelbrain's metal2vulkan on the Mac (rustup target
aarch64-apple-ios, Homebrew llvm@21, SDKROOT the macOS SDK for cargo's build scripts); m2v_test opens it over
PanVK (/redwood/lib/libvulkan_panfrost.dylib):

```
m2v_test: device "Mali-G715 MC7" (class NativeBridgeMetalDevice)
m2v_test: blit fill: 1048576 of 1048576 words right
m2v_test: clear of 1080x2400: 2592000 of 2592000 pixels orange in 3.09 ms
m2v_test: 100 full-screen clears, each committed and waited for: 1.016 ms on average, 0.908 at best
m2v_test: IOSurface written by the CPU, copied by the GPU: 65536 of 65536 pixels green
m2v_test: own pages (no-copy buffer made, texture made) copied by the GPU: 65536 of 65536 pixels red
m2v_test: IOSurface cleared by the GPU, read by the CPU: 65536 of 65536 pixels blue
m2v_test: PASS
```

What it took: PanVK exports vkGetInstanceProcAddr on Darwin (ash::Entry::load_from); VK_EXT_external_memory_host
in PanVK (a host pointer becomes a hakios-panthor descriptor imported as a dma-buf; HOST_ALLOCATION importable;
16 KiB alignment), the IOSurface table (hakios_drm_host_surface, called by the frontend) and kernel requests
0x7e/0x7d behind drmPrimeFDToHandle, the kernel's bind taking surface and client buffers; IOKit mappings made
unique (kIOMapUnique); host image copy off on Darwin (metal2vulkan's host reads of Shared textures faulted in
pan_access_tiled_image: to be found; copies through a submission work); the processor's caches cleaned and
invalidated over imported ranges before each submission and after each wait (the Mali does not snoop: 32 to 646
pixels were stale); the kernel's hang report for the Mali instead of the Adreno's registers (it panicked).

## Step 18: the window server on Metal over PanVK (in progress; runs darwin-ws1..14)

Boot-args `mali=1 metal=2 metal_m2v=1 ca_metallib=/redwood/share/ca/QuartzCore-default.metallib`. backboardd takes
the Mali as its Metal device through HakiosMetal (userland/src/HakiosMetalCA.m built with metal2vulkan) and
PanVK. What each run found, and the fix:

- the system image on the phone has no HakiosMetal.bundle: kernel `metal=2` names it on the RAM disk
  (`../../../redwood/lib/HakiosMetal`), no reflash;
- the plug-in: Turnip's path made PanVK's; hakios-metal's functions left to dynamic lookup (the m2v path
  never calls them); QuartzCore's default.metallib from tools/ca-metallib.py on the Mac; libhakios.dylib
  rebuilt from source (the prebuilt one had no ca_metallib hook);
- PanVK's CRC (transaction elimination) area zeroed past the image's memory: PANVK_DEBUG=no_crc;
- QuartzCore opens default.metallib by index under a mirror directory: newLibraryWithURL/File redirected;
- the iPhone's own fragment functions (display_overdrive_*, harmony, luma_boost, p3_clamp) missing from the
  Mac's library: the copy of the same precision stands in (logged once each);
- raster order groups without fragment interlock: unordered (metal2vulkan-hakios.patch,
  METAL2VULKAN_UNORDERED_ROG), CoreAnimation exits the window server over a refused pipeline;
- a GPU hang (stream 2 waiting forever for a seqno, CS status read after a CSG status update): PanVK relies on
  a new group's CS registers being zero, and every group here is group 0: hakios-panthor clears r0-r91 on
  each stream at GROUP_CREATE; AS1 flushes are now LOCK/FLUSH_MEM/UNLOCK over the whole space;
- the kernel's hang report reads each stream's CS status; the plug-in prints a backtrace on a crash;
- cache maintenance of imported memory moved into the kernel (HakiosGPU cleans and invalidates a client's
  IOSurfaces and own pages by physical address before each Mali submission, as its msm path does): a
  process's own mappings of them can be gone, and user-space cache operations faulted;
- m2v_test keeps a shader cache of its own (one made by root was not the window server's to write).

**Run darwin-ws19: the home screen drawn by the Mali** (docs/darwin-ws19-home-on-mali.png): backboardd ran the
whole 240 s on its one Metal device over PanVK with no restart and no GPU hang; m2v_test beside it passed. The
wallpaper is black and the widgets are white boxes (blur and materials, and the iPhone functions that stand-in
copies replace, still to be looked at).

The wallpaper and widgets are the same on the processor's path (docs/m4-home-screen.png): nothing set as
wallpaper, no widget extensions running. Not the GPU's.

## Step 19: speed (runs darwin-ws20..28)

Kernel `touch_demo=N` (hakios.goodix.c): N seconds after start, a finger of its own swipes the home screen a
page and back every 2 s, 30 times, so frames can be timed with nobody at the phone. MiniIOMobileFramebuffer's
5-second lines count frames; the kernel's `HakiosGPU: Mali:` line counts submissions, cache work and waits, and
`mali: stream N: submission to done` gives each stream's times by power of two; `m2v_telemetry=1` prints
metal2vulkan's own (commit to completion, pipeline creation).

| run | path | frames a second while swiping | an idle frame on the GPU |
|---|---|---|---|
| darwin-cpu20 | processor (no `mali`) | 38-42 | - |
| darwin-ws20 | Mali, PanVK memory non-cacheable | 29-35 | 8-16 ms |
| darwin-ws25 | Mali, all PanVK memory through the GPU's L2 | 39-45 | 4-8 ms |
| darwin-ws28 | Mali, cached except IOSurfaces and client pages | 34-40 | 8-16 ms |

What was found:

- every Mali mapping was MEMATTR slot 5 (0x4c, non-cacheable): each texel and tile write went to DRAM. PanVK's
  memory is now slot 0 (0x88) unless bound MAP_UNCACHED (which hakios-panthor now passes), and the tiler heap
  too; each kernel ring submission ends with WAIT all and an L2/LSC clean before its fence is written. The
  kernel's own rings and fence words stay slot 5. Boot-arg `mali_cached` (bit 0 PanVK, bit 1 heaps).
- with IOSurfaces and client pages (processor-cached memory shared with other processes) also cached by the
  GPU, the screen showed wrong geometry after swiping (darwin-ws26/27: a stray triangle, an icon missing).
  They are mapped non-cacheable for now (`mali_shared_cached=1` to cache them, to find the cause); the cause
  is not known yet.
- the 3 subqueues of a frame are done together; while swiping, frames queue behind each other (64-256 ms
  from commit to completion) because the GPU takes longer than a frame per frame.
- metal2vulkan creates 71 pipelines at every boot, 91 s in all (the longest 11 s): the shader cache is in
  RAM. A cache kept on the phone's storage is the fix (with the step that writes there).
- GPU buffers no longer grow without end in these runs (about 580-690).
- m2v_test: a GPU-cleared IOSurface read by the processor had 32 stale pixels once (darwin-ws23); nothing
  invalidates the processor's caches after the GPU is done. It passed in ws25 and ws28.

The shared-memory corruption, narrowed (runs darwin-ws29..31):

- `mali_shared_cached=1` (IOSurfaces cached): 38-46 frames a second, much corruption (stray icons on the App
  Library); `=2` (client pages cached): 35-41, a label half cut;
- the kernel self-test now checks the L2 itself (a cached word read, changed by the processor, read again by a
  new submission; a GPU store read back after the fence): OK, so the per-submission clean and invalidate works;
- `mali_shared_cached=3 m2v_sync=1` (the plug-in waits for every command buffer at its commit: one frame on the
  GPU at a time): a clean screen, at 25 frames a second. So the corruption needs GPU work and processor writes
  to the same shared memory to overlap.

Codex's reading: an ownership race on shared memory. A partly written cache line that both the GPU's L2 and a
processor own (byte-precise when the GPU does not cache it) can be written back over the other's bytes; and
nothing invalidates the processor's caches after the GPU writes. kbase and Panthor map imports GPU-cached, with
cache maintenance at ownership hand-offs (map/unmap, KCPU import), not per submission. The fix is to track
ownership of shared ranges across clients and hand them over with the right maintenance. Until then IOSurfaces
and client pages stay non-cacheable for the GPU.

Codex's review of the CSF back end (to do): service CS/CSG events (tiler OOM bit 26, faults), grow the tiler
heap on OOM, skip the per-submission L2 flush with Panthor's LATEST_FLUSH id, a poller or interrupts for
completion, the GLB power-off timer.

## Next

8. User space: a Mali back end for hakios-metal (today Adreno PM4 + ir3) emitting CSF command buffers and
   Valhall shaders (Mesa's Valhall compiler in place of ir3), GET_PARAM answers for Mali, interrupts
   instead of polling, address reuse.
9. A triangle (fragment + tiler: REQ_RESOURCE fragment|tiler, RUN_IDVS / RUN_FRAGMENT, framebuffer
   descriptors), the GPU clock up from 150 MHz (ACPM DVFS domain 7), then this path inside XNU and a
   Metal back end.
7. Simple jobs (a fill, a copy), then a triangle; then a Metal back end (HackiOS's hakios-metal, for
   Adreno today) with a Valhall shader compiler.
