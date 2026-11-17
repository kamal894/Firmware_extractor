# First polled CSF store on caiman

Use CSG slot 0, stream 0, and a fresh AS1. Put the command ring and the destination in AS1. Put the firmware-facing queue pages and any normal suspend buffer in AS0. The test succeeds when the CPU reads `0x89abcdef` from the destination PA. The smallest candidate is three CS instructions. The first-run sequence below explicitly selects and waits for a scoreboard slot and publishes one 64-byte ring block.

This is static research for G715 arch 11.8.10 and the r54p3 firmware interface `0x02080000`. **GUESS** marks a proposed setting or behavior whose applicability to this firmware and probe is unverified. A source-only field is identified as such. Binary offsets below are hexadecimal byte offsets from symbols in the supplied `mali_kbase.ko`. The observed boot baseline is `probe-log-gpu6.txt`, with `probe/gpu.c:gpu_firmware`, `docs/mali-boot.md`, and `docs/gpu-plan.md` providing the loader and MMU context.

## Address conventions and discovery

Use the GPU physical MMIO base `0x1f000000`. Interface addresses are **MCU VAs in AS0**. With the CPU MMU off, access their PA backing. For the logged boot, `PA = 0xf00d8000 + (VA - 0x04000000)` within `[0x04000000, 0x04010000)`. Derive the PA from the current run's `shared_pa` in the implementation.

| Control block | Fields at offsets | Slot 0 or stream 0 |
|---|---|---|
| Global at `0x04000000` | Version `+00`, features `+04`, input VA `+08`, output VA `+0c`, group count `+10`, group stride `+14`, perf-counter size `+18`, instrumentation features `+1c` | `0x02080000` after boot, input `0x04002000`, output `0x04006000`, count 8, stride `0xa0` |
| CSG g at `0x04001000 + g * group_stride` | Features `+00`, input VA `+04`, output VA `+08`, normal and protected suspend sizes in bytes `+0c/+10`, stream count `+14`, stream stride `+18` | Control `0x04001000`, input `0x04003000`, output `0x04007000`, count 8, stride `0xc` |
| CS s at `CSG_control + 0x40 + s * stream_stride` | Features `+00`, input VA `+04`, output VA `+08` | Control `0x04001040`, input `0x04003400`, output `0x04007200` |

`parse_capabilities+0x38..+0x11c,+0x1b4..+0x31c,+0x344..+0x3a8` confirms the offsets and the two array bases. The firmware file's shared section entry at `0xf4` gives data range `[0x47000,0x49000)`. The three control blocks start at blob offsets `0x47000`, `0x48000`, and `0x48040`. The static group and stream input/output pointers and strides above come directly from those bytes. The blob initializes version, CSG0 suspend sizes, and CS0 features to zero. Read live control words after boot. The saved log only dumps the global header. For CS features, Panthor defines work-register count as `(features & 0xff) + 1`, scoreboard count as `(features >> 8) & 0xff`, and compute/fragment/tiler capability at bits 16/17/18. Use a stream that exposes at least three work registers and one scoreboard slot for the 32-bit example. [Panthor interface structs](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/panthor_fw.h)

## Global requests and doorbells

| Offset | Input field or output counterpart | Evidence |
|---|---|---|
| `+00` | Input `GLB_REQ`, output `GLB_ACK`. Toggle bits 1 progress timer, 2 `ALLOC_EN`, 3 poweroff timer, and 8 ping. Idle enable bit 10 is a level. r54p3 also toggles bit 16 for the CSG suspend timer. | `global_init+0xd8..+0xf0,+0x13c..+0x190,+0x25c..+0x274`, `kbase_csf_firmware_enable_gpu_idle_timer+0x54..+0xb0`. Ping bit from Panthor. |
| `+04` input | `GLB_ACK_IRQ_MASK`. Controls which interface changes can raise a job interrupt. | `global_init+0x274..+0x2b0` programs platform-dependent mask bits. |
| `+08` | Input `GLB_DOORBELL_REQ`, output `GLB_DOORBELL_ACK`. Bit g selects CSG slot g. | `kbase_csf_ring_csg_doorbell+0x38..+0x68`. |
| `+10/+14` input | `GLB_PROGRESS_TIMER` and `GLB_PWROFF_TIMER`. | `global_init+0x110..+0x120,+0x15c..+0x170`. Progress timeout is shifted right by 10. |
| `+18/+1c` input | `GLB_ALLOC_EN` shader-core mask, low/high. The matching request is bit 2. | `global_init_on_boot+0x4c..+0xb0` obtains the core mask and passes it to `global_init+0xb0..+0xf0`. |
| `+80/+84` input | `GLB_IDLE_TIMER` at `+80`. `+84` bit 0 is the timer scale selector. `+84` bit 9 is separately programmed for the sleep-on-idle policy. | `kbase_csf_firmware_set_gpu_idle_hysteresis_time+0x34..+0x8c`, `kbase_csf_firmware_enable_gpu_idle_timer+0x64..+0xb0`, `global_init+0x190..+0x1ac`. |
| `+88` input | r54p3 poweroff timer companion, bit 0. **GUESS** for this probe, use 0 with a timer scaled by 1024. | `global_init+0x120..+0x134` verifies the write and mask, which are absent from Panthor's base layout. |
| `+90/+94` input | r54p3 CSG suspend timeout and `+94` bit 0 scale selector. | `global_init+0x1b8..+0x274,+0x370..+0x3b4` and ELF `.rodata+0xf2db` (CSG suspend timeout diagnostic). |

For a toggle, first establish that the previous request is acknowledged for that mask. Write payload words, then update only the selected request bits using `new_req = (old_req & ~mask) | ((ack ^ mask) & mask)`. Publish the updates before ringing the doorbell. Poll `((req ^ ack) & mask) == 0` with a deadline. For state or level fields set the requested value directly and poll the corresponding ACK field. The output direction and comparison are confirmed by `kbase_csf_fw_io_global_read+0x0..+0x18`, `kbase_csf_fw_io_global_input_read+0x0..+0x18`, and `wait_for_global_request_with_timeout+0x74..+0xa4`. [Panthor global initialization and timer conversion](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_fw.c.html#1024)

A hardware bell is a 32-bit write of **1** to `0x1f000000 + 0x80000 + n * 0x10000`. Bell 0 is `0x1f080000`. A global configuration request rings bell 0 directly. To notify CSG g, toggle bit g of global doorbell REQ against global doorbell ACK, then ring bell 0. The kernel path for stream s also toggles bit s of the group's doorbell REQ against the group's doorbell ACK before the global step. The direct user kick uses the group's allocated doorbell index in `CS_CONFIG[15:8]`. Kbase allocates an index from a bitmap, saves it per group, and copies it into each bound stream. The group slot and doorbell index are independent. Use index 1 for the sole group, giving `0x1f090000` for a direct stream kick. See `kbase_csf_ring_doorbell+0x20..+0x28` (register ID `0xff+n`), `kbase_regmap_v10_8_init+0x1960..+0x1988` (pointer entries at `0x7f8,0x800,0x808`), `kbase_regmap_v11_init+0x68..+0x6c` (inherits the map), `kbase_csf_ring_cs_kernel_doorbell+0x5c..+0xd8`, `kbase_csf_ring_cs_user_doorbell+0x2c..+0x3c`, `program_csg_slot+0x244..+0x29c`, and `program_cs+0x94..+0xd0,+0x298..+0x2b8`. [Panthor CSF register definitions](https://lkml.iu.edu/hypermail/linux/kernel/2604.1/05425.html)

## CSG and CS messages

All offsets below are bytes from the block's own input or output pointer. A 64-bit configuration write may be made as low then high 32-bit writes before the request is published, as in kbase.

| CSG input | Field and interpretation |
|---|---|
| `+00/+04/+08/+0c` | REQ, ACK IRQ mask, stream doorbell REQ, stream IRQ ACK. State [2:0] is TERMINATE=0, START=1, SUSPEND=2, RESUME=3. Endpoint configuration update toggles bit 4. Status update toggles bit 5 (Panthor). |
| `+18/+1c` | R54p3 writes an additional 64-bit neural allow mask here. Use 0 for G715. The write and interpretation are traced to `program_csg_slot+0x1c..+0x84,+0x210..+0x228,+0x460..+0x484,+0x6a4..+0x6c8` and the `CSG_ALLOW_NEURAL` warning at `.rodata+0xcad1`. |
| `+20/+28/+30` | `ALLOW_COMPUTE` u64, `ALLOW_FRAGMENT` u64, `ALLOW_OTHER` u32 (tiler mask in Panthor). |
| `+34` | 32-bit endpoint configuration. Compute cap [7:0], fragment cap [15:8], tiler cap [19:16], group priority [31:28]. Panthor defines exclusive compute/fragment at bits 20/21. |
| `+38` | Alternate extended endpoint field on newer implementations. Panthor selects it from interface 4.0.0. Kbase uses a separate device-version test and otherwise writes `+34` and clears `+38`. Use `+34` on this target. |
| `+40/+48` | Normal and protected suspend-buffer MCU VAs, both u64. |
| `+50` | `CSG_CONFIG` receives the hardware AS number **unshifted**. Write `0x00000001` for AS1. |
| `+54` | Secondary configuration word, trace configuration in Panthor. Use 0 for this job. Kbase also has conditional writes at `+58` and `+60/+64`; their semantics and relevance here are **GUESS**. Leave their boot defaults for this example. |

CSG output has ACK at `+00` and stream doorbell ACK at `+08`. Panthor names `+0c` stream IRQ REQ and `+10/+14/+18/+1c` current endpoint, requested endpoint, group state, resource dependency. CSG ACK state 1 indicates group start. Kbase startup writes the endpoint payload with the other group fields and publishes `START` via the state mask. For an endpoint change on a resident group, kbase toggles bit 4 against ACK and rings the group. Panthor also toggles bit 4 with its initial START. For the first pass below use the kbase initial START path. See `program_csg_slot+0x174..+0x198,+0x40c..+0x634`, `update_csg_slot_priority+0x138..+0x184`, and `kbase_csf_ring_cs_kernel_doorbell+0x68..+0x98`. [Panthor group scheduling](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_sched.c.html#2389)

The AS number comes from the context retained by `kbase_ctx_sched_retain_ctx`; kbase loads it from the same context word at `program_csg_slot+0x18c..+0x198` and `+0x560..+0x57c`, where it goes straight to input `+0x50`. Panthor likewise writes `panthor_vm_as(group->vm)` directly. Use separate AS1 page tables for execution. AS0 already services the firmware, and AS1 is present in gpu6's `AS_PRESENT=0xff`. The hardware outcome of assigning AS0 to a user CSG is **GUESS** and unnecessary for this path. [Panthor group programming](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_sched.c.html#1455)

**Suspend buffers.** If live `suspend_size` is nonzero, provide a zeroed, page-aligned AS0 buffer covering the advertised size. Kbase binds an MCU mapping and supplies its VA for the normal buffer. Panthor allocates the buffer in its firmware VM. **GUESS** whether firmware accepts a null normal pointer with nonzero advertised size on START, even if the host never requests suspension. Zero is the candidate when the live size is zero. Keep the protected pointer at zero for this ordinary stream, never request protected mode, and treat acceptance of zero with a nonzero protected size as **GUESS** on this firmware. Kbase's protected binding is a separate path. See `kbase_csf_mcu_shared_group_bind_csg_reg+0xd4..+0x178`, `kbase_csf_mcu_shared_group_update_pmode_map`, and `program_csg_slot+0x580..+0x5d0`. [Panthor suspend buffer allocation](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_fw.c.html#519)

| CS input | Field and interpretation |
|---|---|
| `+00/+04/+0c` | REQ, CONFIG, ACK IRQ mask. REQ state [2:0] is STOP=0, START=1. CONFIG priority [3:0], bell [15:8]. Panthor labels REQ bit 4 extract event, bits 8/9/10/11 idle indications, and bits 30/31 fatal/fault. Kbase also writes REQ bit 12 at startup, meaning **GUESS** here. |
| `+10/+18` | 64-bit ring base in **AS1**, 32-bit size in bytes. |
| `+20/+28` | Heap fields in Panthor. Leave boot defaults for the non-tiling stream. |
| `+30/+38` | 64-bit pointers to separate queue input and output pages in **AS0**. |
| `+40/+44` | Instrumentation config and size. Set both zero when unused. |

CS output ACK is `+00`. Panthor guides for diagnosis are command pointer `+40`, wait status `+48`, scoreboards `+5c`, blocked reason `+60`, FAULT/FATAL `+80/+84`, and their u64 details at `+88/+90`. CS REQ vs ACK differences in bits 31/30 flag new fault/fatal events. The CS output interface is distinct from the queue output page. Queue input `+00` is u64 INSERT and `+08` is u64 EXTRACT_INIT. Queue output `+00` is firmware's u64 EXTRACT and `+08` is its u32 ACTIVE. Fresh queue backing starts zero. On resume, copy output EXTRACT to input EXTRACT_INIT before resuming. INSERT and EXTRACT are advancing **byte cursors**. With a power-of-two ring, the byte index is cursor `& (size - 1)`. Kbase validates the queue VA pair and CS input offsets in `program_cs+0x1d0..+0x2bc,+0x38c..+0x3d4,+0x4e8..+0x560`. Panthor supplies the queue-page fields and cursor arithmetic. [Panthor interface structs](https://raw.githubusercontent.com/torvalds/linux/master/drivers/gpu/drm/panthor/panthor_fw.h) · [Panthor queue submission](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_sched.c.html#3322)

## CS words

Each command is one little-endian u64. Number registers by their 32-bit register index, use an even index for a pair, and encode the pair by its first register (e.g., r0:r1 is 0). Unlisted bits are zero. These are Mesa **arch 11** XML encodings. The kbase submission path treats the ring as opaque. Executability of the resulting program on the supplied firmware is untested.

| Instruction | Encoding |
|---|---|
| MOVE / MOVE48 | Opcode `0x01` at [63:56], destination pair's first register [55:48], unsigned immediate [47:0]. |
| MOVE32 | Opcode `0x02`, destination [55:48], immediate [31:0]. |
| STORE_MULTIPLE | Opcode `0x15`, first data register [55:48], address pair's first register [47:40], register mask [31:16], signed byte offset [15:0]. Mask 1 stores one word. Mask 3 stores two consecutive words, starting at the address plus offset. |
| SET_STATE_IMM32 | Opcode `0x1c`, state [39:32], value [31:0]. State 1 (`SB_SEL_OTHER`) selects the load/store scoreboard slot. |
| WAIT | Opcode `0x03`, scoreboard mask [31:16]. Use mask 1 for slot 0, immediate mode (bit 33 clear). |
| FLUSH_CACHE2 | Opcode `0x24`, defer mode [52], signal slot [51:48], latest-flush-ID **register number** [47:40], wait mask [31:16], other-cache mode [11:8], LSC mode [7:4], L2 mode [3:0]. LSC/L2 modes 0 none, 1 clean, 3 clean/invalidate. Other mode 0 none or 2 invalidate. |
| NOP | Zero word. |

For a fresh uncached target, try the store plus WAIT with no cache flush. **GUESS** that the AS1 MEMATTR=0x4c mapping and the store's memory path make the result visible to the noncacheable CPU reader without a flush. If a later test needs explicit writeback, read GPU MMIO `0x1f010000` (LATEST_FLUSH_ID), insert `MOVE32 r3,id` after the store WAIT, then `0x2400030000000233` (clean/invalidate L2 and LSC, invalidate other, signal slot 0) and another WAIT on slot 0. This is the Panthor encoding with r3 as the operand. It requires at least four work registers. With an executable cacheable ring, follow the platform's code publication requirements as well. [Mesa v11 XML](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/06efd8542f442e701685e26abe4d73c4ddf7b249/src/panfrost/genxml/v11.xml) · [Mesa cs_builder.h: cs_store, cs_set_state_imm32, cs_flush_caches](https://fossies.org/linux/mesa/src/panfrost/genxml/cs_builder.h) · [Mesa panvk queue scoreboard setup](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/09eccf7f48d780fa0337c05e5ccdb84810182b44/src/panfrost/vulkan/csf/panvk_vX_gpu_queue.c) · [Panthor prepare_job_instrs](https://codebrowser.dev/linux/linux/drivers/gpu/drm/panthor/panthor_sched.c.html#3187)
