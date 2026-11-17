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

## Next

6. A triangle (fragment + tiler: REQ_RESOURCE fragment|tiler, RUN_IDVS / RUN_FRAGMENT, framebuffer
   descriptors), the GPU clock up from 150 MHz (ACPM DVFS domain 7), then this path inside XNU and a
   Metal back end.
7. Simple jobs (a fill, a copy), then a triangle; then a Metal back end (HackiOS's hakios-metal, for
   Adreno today) with a Valhall shader compiler.
