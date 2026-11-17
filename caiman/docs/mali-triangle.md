# First triangle on the Mali-G715: notes (in progress)

Sources: Mesa main (2026-11) `src/panfrost/genxml/v11.xml`, `src/panfrost/vulkan/csf/panvk_vX_cmd_draw.c`,
`panvk_vX_gpu_queue.c`. Line numbers are of that checkout (scratchpad/mesa). Nothing here is tested yet.

## CS instructions (v11.xml)

| Instruction | Opcode | Fields |
|---|---|---|
| REQ_RESOURCE | 34 (0x22) | bit 0 compute, 1 fragment, 2 tiler, 3 IDVS, 4 RT |
| RUN_IDVS | (CS Opcode RUN_IDVS) | 31:0 flags override, 32 progress, **33 malloc enable**, 34 draw-id reg, 35-39 varying/fragment SRT/FAU/TSD select, 47:40 draw id |
| RUN_FRAGMENT | (RUN_FRAGMENT) | 0 enable TEM, 7:4 tile order, 32 progress |
| FINISH_TILING | (FINISH_TILING) | 32 progress |
| FINISH_FRAGMENT | (FINISH_FRAGMENT) | 0 increment completed, 31:16 wait mask, 39:32 last heap chunk reg, 47:40 first heap chunk reg, 51:48 signal slot, 52 defer |
| HEAP_SET | (HEAP_SET) | 47:40 register pair holding the heap context address |
| SET_STATE_IMM32 | 0x1c | 39:32 state (0 SB_SEL_ENDPOINT, 1 SB_SEL_OTHER, 2 SB_SEL_DEFERRED, 8 SB mask stream, 9 SB mask wait) |

## Staging registers

IDVS SR: SRT 0/2/4/6, FAU 8/10/12/14, SPD 16/18/20/22, TSD 24/26/28/30, GLOBAL_ATTRIBUTE_OFFSET 32,
INDEX_COUNT 33, INSTANCE_COUNT 34, INDEX_OFFSET 35, VERTEX_OFFSET 36, INSTANCE_OFFSET 37, DCD2 38,
INDEX_BUFFER_SIZE 39, TILER_CTX 40 (pair), SCISSOR_BOX 42 (pair), LOW/HIGH_DEPTH_CLAMP 44/45, OQ 46,
VARY_SIZE 48, BLEND_DESC 50 (pair, address | count), ZSD 52, INDEX_BUFFER 54, TILER_FLAGS 56 (Primitive
Flags), DCD0 57, DCD1 58, PRIMITIVE_SIZE 60, TILER_FLAGS2 61.

FRAGMENT SR: FBD_POINTER 40 (pair), BBOX_MIN 42, BBOX_MAX 43, TEM_POINTER 44, TEM_ROW_STRIDE 46.

panvk writes them in prepare_draw and friends: BLEND_DESC `cmd_draw.c:1151`, SCISSOR_BOX `:1191/:1282`,
depth clamps `:1294`, PRIMITIVE_SIZE `:1345`, TILER_CTX `:1570`, ZSD `:2650`, OQ `:2712`, DCD0/1/2
`:3011-3021`, TILER_FLAGS/FLAGS2 `:3093/:3101`, VARY_SIZE `:3200`, counts `:3589-3591`, RUN_IDVS `:3642+`,
FINISH_TILING `:4744`, FINISH_FRAGMENT `:5189`.

## Descriptors (v11.xml)

- **DCD Flags 0** (DCD0): 0 allow FPK, 1 allow FPK to be killed, 3:2 pixel kill op, 5:4 ZS update op,
  6 allow primitive reorder, 16 front face CCW, 17/18 cull front/back, 19 multisample, 23 scissor to bbox ...
- **DCD Flags 1** (DCD1): 15:0 sample mask, 26:16 render target mask.
- **DCD Flags 2** (DCD2): 11:0 read mask, 23:12 write mask, 24/25 no shader stencil/depth read.
- **Primitive Flags** (TILER_FLAGS): 3:0 draw mode, 10:8 index type, 16/17 low/high depth cull (default
  1), 23:22 position FIFO format, 31:24 view mask.
- **Tiler Context** (TILER_CTX points at it): +0 polygon list, word 2: 12:0 hierarchy mask, 15:13 sample
  pattern; word 3 FB width-1 / height-1; word 4 layer count-1; +6 heap (address of a Tiler Heap
  descriptor); word 8 geometry buffer (address >> 12, size >> 12 in 11:0); +10/+12 completed top/bottom;
  +24.. private state.
- **Tiler Heap**: word 0 type 9 (buffer) | buffer type (tiler heap) << 4; word 1 size; +2 base; +4 bottom;
  +6 top. panvk (`gpu_queue.c:772`): size = chunk size, base = first chunk, bottom = base + 64, top =
  base + size. The chunks and the "heap context" given to HEAP_SET come from the kernel
  (DRM_IOCTL_PANTHOR_TILER_HEAP_CREATE); the geometry buffer is 64 KiB 4 KiB after the heap descriptor
  (`gpu_queue.c:460`). Plan: one large pre-allocated chunk and no HEAP_SET, hoping no out-of-memory event.
- **Framebuffer Parameters**: word 8 width-1 / height-1; words 9-10 bound min/max; word 11 sample count
  (log2), sample pattern, tie-break, effective tile size (log2), render target count-1 (22:19), colour
  buffer allocation (>> 10); word 12 Z/S enables (all off); +14 tiler (address of the tiler context).
- **Render Target** (RGB): writeback mode, internal format, writeback format (RGBA8), swizzle, row stride,
  clear colour; 64 bytes each after the framebuffer.

## Shaders (drafts by Codex before its usage limit: probe/gpu_tri_pos.asm, gpu_tri_frag.asm; not assembled)

Position shader: x/y from the vertex id (r60), position written with STORE.i128.istream at the address
LEA_BUF_IMM gives for the internal id (r59), table 0x3d. Fragment shader: constant red, ATEST then BLEND
to RT 0 with blend_descriptor_0.

## Still to read

panvk's build_dcd_flags, the tiler context fill (get_tiler_desc), the fragment pass (framebuffer
descriptor emit in pan_desc / panvk fb code), the blend descriptor for "replace", VARY_SIZE for a
position-only draw, and the exact RUN_IDVS flags for malloc mode.
