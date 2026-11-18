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

## Next

8. User space: a Mali back end for hakios-metal (today Adreno PM4 + ir3) emitting CSF command buffers and
   Valhall shaders (Mesa's Valhall compiler in place of ir3), GET_PARAM answers for Mali, interrupts
   instead of polling, address reuse.
9. A triangle (fragment + tiler: REQ_RESOURCE fragment|tiler, RUN_IDVS / RUN_FRAGMENT, framebuffer
   descriptors), the GPU clock up from 150 MHz (ACPM DVFS domain 7), then this path inside XNU and a
   Metal back end.
7. Simple jobs (a fill, a copy), then a triangle; then a Metal back end (HackiOS's hakios-metal, for
   Adreno today) with a Valhall shader compiler.
