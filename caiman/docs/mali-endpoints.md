# Mali-G715 CSF endpoints in r54p3

Local evidence from `mali_kbase.ko` (`llvm-objdump -dr`), `probe/gpu.c`, the existing boot and submission notes, and gpu10/gpu12 logs. Symbol offsets are hexadecimal byte offsets. Interface offsets are bytes relative to the identified input or output page. **Inferred** marks interpretations or proposed probe changes beyond the read/write evidence.

## 1. CSG slot start and endpoint request

| CSG input | r54p3 programming before START | Binary citation |
|---|---|---|
| `+0x18/+0x1c` | 64-bit neural allow mask. Zero on the path without neural support. | `program_csg_slot+0x404–0x484,+0x6a4–0x6c8` |
| `+0x20/+0x24`, `+0x28/+0x2c`, `+0x30` | 64-bit compute and fragment allow masks, then 32-bit other/tiler allow mask. | `program_csg_slot+0x40c–0x45c,+0x488–0x498` |
| `+0x34`, `+0x38` | Legacy endpoint word at `+0x34`. Explicitly clear `+0x38`. The other branch uses a **64-bit** endpoint word at `+0x38`, gated by a device-version comparison against `0x000e0803`. **Inferred** from the device-version gate and the G715 arch 11.8.10 identification in the local notes, caiman takes the legacy branch. | `program_csg_slot+0x4b0–0x55c` |
| `+0x54`, conditional `+0x58` | Secondary configuration from group object `+0x64`. Panthor calls `+0x54` trace configuration. The `+0x58` write is gated by the newer device version. | `program_csg_slot+0x49c–0x4d0` |
| `+0x50` | Unshifted hardware AS number, so 1 for AS1. | `program_csg_slot+0x18c–0x198,+0x560–0x578` |
| `+0x40/+0x44`, `+0x48/+0x4c` | Normal and protected suspend buffer VAs. | `program_csg_slot+0x57c–0x5cc` |
| Conditional `+0x60/+0x64` | Written when the group object field at `+0x268` is nonzero. Purpose unverified. | `program_csg_slot+0x5d0–0x5f8` |
| `+0x04`, then `+0x00` | ACK IRQ mask `0xffffffff`, then REQ state mask `0x7` with START=1 for a fresh group or RESUME=3. Ring the CSG via the global doorbell. Bound streams are programmed earlier. | `program_csg_slot+0x2ac–0x2fc,+0x5fc–0x634` |

| Legacy endpoint bits | Evidence |
|---|---|
| `[7:0]` compute, `[15:8]` fragment | kbase packs the two byte caps, each clamped to the device limit. `program_csg_slot+0x70–0xb4,+0xf8,+0x4d4–0x4fc`. The C/F order matches the binary dump headings and the local Panthor notes. |
| `[19:16]` tiler | Legacy dump decodes four bits. Startup inserts a boolean derived from the tiler count, so this path encodes 0 or 1. `program_csg_slot+0xc0,+0xf0,+0x4d4–0x4f4`, `kbasep_csf_csg_active_dump_group+0x20c–0x210`. |
| 20 exclusive compute, 21 exclusive fragment | Input definitions from the Panthor layout cited in `mali-csf-submit.md`. The r54p3 output dump tests those bits in the requested word and prints C/F under Exclusive. This startup packer leaves both clear. `kbasep_csf_csg_active_dump_group+0x90–0x94,+0xec–0xf4`, `program_csg_slot+0x4d4–0x500`. |
| `[31:28]` priority | Shift by 28 on startup and replace the same nibble on resident priority change. `program_csg_slot+0x4f8–0x500`, `update_csg_slot_priority+0x114–0x148`. |

`0x10010707` encodes C=7, F=7, T=1, priority 1, and no exclusivity. Initial START updates only state bits. The separate resident-group update writes the endpoint word, toggles REQ bit 4 against ACK, and rings the CSG (`update_csg_slot_priority+0x144–0x17c`). The bit 4 ACK in gpu12 confirms that handshake. **Inferred** from the separate status path below, bit 4 ACK does not establish that the status words were refreshed.

## 2. Post-boot global requests, power and status doorbells

| GLB input | r54p3 behavior | Binary citation |
|---|---|---|
| `+0x18/+0x1c` | Core allocation mask, low/high. Toggle `GLB_REQ` bit 2. Boot sources the mask from PM policy. | `global_init_on_boot+0x4c–0xac`, `global_init+0xb0–0xec` |
| `+0x14`, `+0x88[0]` | Poweroff timer and scale selector. Toggle bit 3. The probe's bit 2 ACK covers allocation only. | `global_init+0xf0–0x150`, `kbase_csf_firmware_update_core_attr+0x140–0x174` |
| `+0x10` | Progress timer, encoded from a host timeout shifted right by 10. Toggle bit 1. | `global_init+0x154–0x18c` |
| `+0x80`, `+0x84[0,9]` | Idle interval and scale. Idle timer enable is a level at REQ bit 10. Boot separately programs `+0x84[9]` for sleep policy. | `kbase_csf_firmware_enable_gpu_idle_timer+0x54–0xac`, `global_init+0x190–0x1a8` |
| `+0x90`, `+0x94[0]` | CSG suspend timeout and scale. Toggle bit 16. | `global_init+0x1b8–0x270,+0x370–0x3b4` |
| `+0x04` | ACK IRQ mask, base `0x4c810f1e`, with conditional bits and preserved bits. | `global_init+0x274–0x2ac` |

GLB REQ/ACK are at input/output `+0x00`. Boot rings hardware bell 0, then waits with mask `0x10c0e` (bits 1, 2, 3, 10, 11, 16). This is a comparison mask, not a mask to XOR into REQ. `global_init+0x2b8–0x2c4`, `global_init_on_boot+0xdc–0xf0`, `wait_for_global_request_with_timeout+0x74–0xa4`. The power-related requests identified in this path are the allocation mask and poweroff timer. A separate GLB shader-power-on toggle or a timer prerequisite for compute was not established.

**Shader power.** The cold loader waits for L2 before MCU start (`kbase_csf_firmware_load_init+0xbf0`, also see `mali-boot.md`). Kbase also implements conditional host shader control **after** MCU global reinit. The PM branch at `kbase_pm_update_state+0x5a8–0x5dc` either goes to `+0xb84` without issuing shader power there, or reaches the host-control path at `+0x460–0x478` (or its core-stack branch), issuing a policy-mask power-on through `kbase_pm_invoke`. The legacy `SHADER_PWRON` mapping is MMIO `0x180` (see `mali-boot.md`, `kbase_pm_invoke+0x2fc–0x31c` and `kbase_regmap_v10_8_init+0x1228–0x1248`). **Inferred** the no-host-write branch delegates shader power to firmware. The static evidence here does not establish the runtime ownership branch on this Pixel. For a firmware-owned trial, omit the probe's unconditional host shader pre-power and avoid host shader power writes after boot as well. For host-controlled power, reproduce the gated PM sequence. The saved logs already show all seven shader cores ready, so the observed stall does not show a lack of physical shader power.

**Status snapshot.** For resident groups kbase first rings the group's allocated hardware bell, writes CSG REQ bit 5 to the inverse of CSG ACK bit 5, then rings the selected CSG slots and waits for REQ/ACK bit 5 equality (`kbase_csf_csg_update_status+0xcc–0xe4,+0x184–0x1b8,+0x234–0x290`, `csg_slots_status_update_finish+0x94–0xac`). For slot 0, the slot notification flips bit 0 of GLB input doorbell REQ `+0x08` against GLB output doorbell ACK `+0x08` and rings hardware bell 0 (`kbase_csf_ring_csg_doorbell+0x38–0x68`). This refresh is used for diagnostics (`kbasep_csf_csg_active_dump_print+0x5c`). START and submission do not use it to grant endpoints.

## 3. CSG output layout and interpretation

| CSG output | Meaning on the legacy path | Binary citation |
|---|---|---|
| `+0x00` | CSG ACK. State handshake in `[2:0]`, 1 for START. Also holds matched config/status handshake bits. | `update_csg_slot_status` and `csg_slots_status_update_finish+0x94–0xac` |
| `+0x08` | Stream doorbell ACK bitmap. | `kbase_csf_ring_cs_kernel_doorbell+0x68–0x98` |
| `+0x10` | Currently allocated endpoints. | `kbasep_csf_csg_active_dump_group+0x50–0x78,+0x1ec–0x210` |
| `+0x14` | Currently requested endpoints. This is the firmware's status value. The dump decodes both endpoint words as C `[7:0]`, F `[15:8]`, T `[19:16]`, printed as Alloc/Req (ELF `.rodata+0xa46d`). | `kbasep_csf_csg_active_dump_group+0x7c–0x94,+0x1dc–0x240` |
| `+0x18` | Group status/state flags. Bit 0 is idle, printed Y/N. The dump's separate Run State column comes from the host group object, and the START ACK is at `+0x00`. Other flag meanings unverified here. | `kbasep_csf_csg_active_dump_group+0xf8–0x12c,+0x1ec–0x240` |
| `+0x0c`, `+0x1c` | Stream IRQ REQ and resource dependency, respectively, per Panthor as summarized in `mali-csf-submit.md`. | Source layout via local notes |

**Inferred** gpu10/gpu12 show the output addresses are plausible, but neither run records a bit 5 refresh. In gpu12 the endpoint print after bit 4 ACK is also before compute submission. Sample with a completed bit 5 handshake while the dispatch is pending before interpreting zero allocations. The logs establish extract at byte 160 and an untouched destination. The extract cursor alone cannot distinguish a blocked dispatch from a fault elsewhere in its execution path.

## 4. CS input and RUN_COMPUTE

| CS input | r54p3 behavior | Binary citation |
|---|---|---|
| `+0x04` | CONFIG = stream priority `[3:0]` plus allocated group doorbell index `[15:8]`. No iterator selector in this write. | `program_cs+0x298–0x2b8` |
| `+0x10/+0x14`, `+0x18`, `+0x30–0x3c` | Ring VA, ring size, and the two queue page VAs. | `program_cs+0x1d0–0x294` |
| `+0x40/+0x44`, `+0x0c` | Zero instrumentation configuration/size when disabled. ACK IRQ mask `0xffffffff`. | `program_cs+0x38c–0x3d0` |
| `+0x00` | The final write uses mask `0xc8001507`, ORs in `0x1500` (bits 8, 10, 12), supplies the stream state, copies ACK bits 30/31 and selects bit 27 from ACK or its inverse according to a host flag. Then it rings the stream through the kernel doorbell path. | `program_cs+0x4e8–0x560` |

Bit 12 is set in the generic path. Its semantics and any special dependency of RUN_COMPUTE on it were not determined. The generic path programs queue transport and does not select a compute iterator or scoreboard in CS_CONFIG. **Inferred** from the Mesa command encoding notes in `mali-csf-submit.md` and Step 5 of `gpu-plan.md`, validate iterator/scoreboard selection, dispatch operands and shader descriptors at the command-stream level. The probe's `SB_SEL_OTHER` setup is for load/store. Its presence does not validate the compute scoreboard. gpu12's `CS0_FEATURES=0x0007105f` advertises compute at bit 16 per the Panthor layout in the local notes.

## 5. Ordered probe changes

1. **Inferred** for a firmware-owned boot comparison, remove the unconditional `SHADER_PWRON` from the pre-MCU helper for this path. Keep the L2-ready prerequisite. Record initial and in-flight shader READY/PWRTRANS. Keep this ownership comparison separate from the CS/CSG changes.
2. **Inferred** in `gpu_first_job`, use the live GLB pointers from the capability header. Keep the acknowledged allocation mask and add the boot progress, poweroff and suspend timer setup with matched scale flags and a bounded ACK wait. Keep idle/sleep disabled for the active probe. Derive timer values from the selected clock and policy, since no caiman-specific encoded values were established here.
3. **Inferred** program the CS payload, IRQ mask and generic startup REQ fields from §4, then the CSG fields in §1. Keep the normal suspend backing in AS0 (gpu12 advertises 26,368 bytes). Use `+0x34=0x10010707` and `+0x38=0`. Publish state START and notify via the stream/group hierarchy with a barrier before bell 0. Require both START ACKs and the original MOVE/STORE check before compute.
4. **Inferred** remove the mirrored write to `+0x38` and the unchanged-payload bit 4 toggle from `gpu_first_compute`. Populate the compute code and descriptor mappings before AS1 activation where possible. If installed later, publish the page-table edits and use the AS1 MMU maintenance sequence before submitting. Publish INSERT, set the stream and CSG doorbell bits against their ACKs, then ring bell 0. Use a separate bit 5 status snapshot before submission and again while stalled. Label snapshots stale if the refresh times out.
5. **Inferred** log both endpoint words, group flags, CS command/wait/blocked/fault state, and AS1 fault status/address. gpu10/gpu12 already show `MMU_IRQ_RAWSTAT=0x00020000` at group start, while `gpu_faults()` prints only AS0 fault details. Treat that raw IRQ as unresolved until the relevant AS status is read. If a fresh snapshot shows compute endpoints allocated, check the RUN_COMPUTE operands, scoreboard selection, descriptor and shader VA accesses against the local Mesa references. The evidence does not identify a single root cause.
