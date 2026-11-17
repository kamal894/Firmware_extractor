# AOC IPC and touch-bus negotiation — static findings

Read-only analysis, 2026-11-17. No device access or target execution. Read `aoc-boot-map.md` (including §§10–12) and `aoc-fw-load.md` first. Only this report was written.

## Evidence and scope

Binary citations use `A` = supplied `aoc_core.ko`, `T` = `touch_bus_negotiator.ko`, `D` = `aoc_tbn_service_dev.ko`, `G` = `goog_touch_interface.ko`. `function+offset` means offset from the symbol, with `.text` section address sometimes included for locating it in `scratchpad/aoc/aoc_core.dis`. These are relocatable ELF section addresses, **not runtime physical addresses**. Other modules were disassembled in memory with `llvm-objdump -dr`. All integers below are little-endian.

SHA-256 identities:

| Input | SHA-256 |
|---|---|
| A | `1a348d8c60b0f70363fb66a8b255c9ea320e8144781ec819c4a971eb1ef9ad50` |
| D | `b81e2908543d91da8a6cea437445bef76bb3b80c84dc623f7eb789948c332cf8` |
| T | `802e3a4f3ad2109f2b121267c6e26234dc0d8cfc85e28fb2b51cb4db6a171759` |
| G | `6b3cdf258609669f539c12a0e1364a14abb5cd62bdcd32835bdd0e98406533a2` |
| aoc.bin | `cba2423541a1b2467f8324dfdcace38804e5404970a65aecc6dad9b53eab9c96` |

Two supplementary source references:

* **H**: [aoc_ipc_core_internal.h, android-13.0.0_r0.85](https://android.googlesource.com/kernel/google-modules/aoc-ipc/+/refs/tags/android-13.0.0_r0.85/aoc_ipc_core_internal.h), blob `454671e86cb50ba6308119dd28945b8c8a989253`. Source names explain the binary layout; matched accesses are cited below. Fields not exercised by the binary paths are explicitly source-only.
* **M**: [mailbox-wc.c, commit 751036690ecd31bd8854638966b9d585ded74bbd](https://android.googlesource.com/kernel/google-modules/aoc/+/751036690ecd31bd8854638966b9d585ded74bbd/mailbox-wc.c), blob `735d9a81ef8d48ba0a2674e2a260f5dfc4ce001a`. **The mailbox-controller implementation is not in the supplied binaries. Its MMIO recipe below is source-based, not binary-verified for this exact build.** The supplied DT selects its compatible string. HTTPS curl failed DNS resolution; indexed Gitiles source was available through web lookup. No source files were saved.

## 1. Online detection and timing

Let **C = 0x95c00000**, the AP physical IPC base from the boot notes. Linux sets both its global `aoc_control` pointer and private `ipc_base` to carveout mapping + firmware IPC offset. [A:aoc_fw_callback+0x5ac–0x5d4, text 0x850–0x878; +0x6e4–0x6fc, text 0x988–0x9a0.]

`aoc_fw_ready()` requires a non-NULL control pointer and exactly `u32[C+0] == 0xa0c00a0c`. It does **not** test C+4, C+8, C+0xc, or a host-ready word. [A:aoc_fw_ready+0x0–0x28, text 0x94–0xbc.]

Linux's startup transition is interrupt-driven:

1. Firmware callback stores Linux-only state **1** before starting AOC. [A:aoc_fw_callback+0x6c0–0x6d0, text 0x964–0x974.]
2. A mailbox RX callback in state 1 checks that same magic. If valid, it stores Linux-only state **2**, then queues the online work item. The incoming mailbox payload is not consulted for this decision. No particular channel number is tested in this startup branch. [A:aoc_mbox_rx_callback+0x1c–0x74, text 0x2d24–0x2d7c.]
3. Online work cancels the delayed startup guard, obtains/validates services, and changes Linux-only state to **3** before device registration. It rejects zero services, counts >=201, invalid names, duplicates, and invalid service descriptors. [A:aoc_did_become_online+0x3c–0x60, +0x90–0x1bc, +0x368–0x3b8; text 0x3b44–0x3b68, 0x3b98–0x3cc4, 0x3e70–0x3ec0.]

**State 1/2/3 lives in Linux's `aoc_state`, not in C+0xc.** Merely polling C+0 for magic is the bare-metal equivalent of the firmware-ready predicate; it does not recreate Linux device objects. [A:aoc_mbox_rx_callback+0x1c–0x70; A:aoc_fw_ready.]

`aoc_monitor_online` is a **one-shot timeout guard**, not a magic/heartbeat polling loop. If Linux state remains 1 it logs failure and requests restart unless DT contains `skip-monitor-online-reset`. It never reads the IPC words itself. Startup queues it with **1250 jiffies**, then separately sleeps **2000 ms** before re-enabling the CPU power mode. HZ is not established by these inputs: 1250/HZ seconds (5 s if HZ=250). [A:aoc_monitor_online+0x10–0x74, text 0x4034–0x4098; A:aoc_fw_callback+0x73c–0x764, text 0x9e0–0xa08.]

Mailbox channels are all requested by numerical DT index; callback context records that index. Once online, RX scans services whose stored mailbox index equals the callback index, invokes their handler or wakes blocked readers/writers after checking queues. [A:aoc_platform_probe+0x44c–0x53c, text 0x2324–0x2414; A:aoc_mbox_rx_callback+0x78–0x178, text 0x2d80–0x2e80.]

DT lines 13403–13453 provide three controllers: PA `0x15200000`, `0x15220000`, `0x15240000`, DT SPI numbers `0x4d`, `0x4e`, `0x4f` (GIC INTIDs 109,110,111 after the standard SPI+32 translation). Global mailbox indices 0–15,16–31,32–47 select their respective local channels 0–15. The binary's service IRQ field is only five bits: do not assume a TBN descriptor can select indices 32–47. [DT cited lines; A:aoc_service_irq_index+0x0–0x8.]

The separate watchdog IRQ is DT SPI `0x82` (GIC INTID 162), named `watchdog`. Its handler calls `trigger_aoc_ssr(0,NULL)`; it is a crash/restart path, not online confirmation. `aoc_watchdog` is the restart work item. [DT lines 13443–13444; A:watchdog_int_handler+0xc–0x18, text 0x9a68–0x9a74; A:aoc_platform_probe+0x558–0x580, text 0x2430–0x2458.]

## 2. Control block and service descriptors

### Control at C

| Offset / AP PA | Width | Meaning and evidence |
|---|---|---|
| +0 / `0x95c00000` | u32 | Ready magic `0xa0c00a0c`; A:aoc_fw_ready+0xc–0x1c. |
| +4 / `0x95c00004` | u32 | Firmware version; paired revision read A:revision_show+0x30 (text 0x4e7c); name from H. |
| +8 / `0x95c00008` | u32 | Hardware version; same paired read. Observed `0x00041a53` belongs here, not a queue pointer. |
| +0xc / `0x95c0000c` | u32 | `status` in H; source-only meaning, no readiness test identified. |
| +0x10 / `0x95c00010` | u64 | `supported_features` in H; source-only meaning. |
| +0x18 / `0x95c00018` | u32 | Service count; A:aoc_num_services+0x14–0x1c. |
| +0x1c / `0x95c0001c` | u32 | Descriptor stride; A:service_at_index+0x44–0x50. |
| +0x20 / `0x95c00020` | u32 | Descriptor-table byte offset **relative to C**, same instructions. |
| +0x24 | 48 bytes | Firmware version name, H source-only. |
| +0x54 | 48 bytes | Hardware version name, H source-only. |
| +0x84 / `0x95c00084` | u64 | System clock offset; A:clock_offset_show+0x58–0x68 (text 0x516c–0x517c); A:aoc_clock_show+0x94–0xa8, +0xc8–0xe0 subtracts it from AP counter then divides by DT divider. |

H's declaration is packed/aligned(4), total 0x8c bytes. There is **no `host_ready` member in that declaration**. Source-only names/layout are supplementary, not proof of the meaning of every firmware-produced value. [H:struct aoc_control_block, source lines 62–74; Gitiles rendered lines 66–78.]

For `0 <= N < count`, descriptor address:

```
S(N) = C + read32(C+0x20) + N * read32(C+0x1c)
```

This uses published stride, not a fixed compile-time table location. The binary's helper accidentally admits N==count; use the strict bound. [A:service_at_index+0x2c–0x50, text 0x8140–0x8164.]

A descriptor has a 32-byte name at S+0; name helper requires byte31==0. Flags are u32 at S+0x20. `(flags & 0x70000)` selects queue=0, ring=0x10000, buffer=0x20000. Mailbox index is `(flags >>19)&31`. Queue/ring distinction is essential; TBN recipe below requires queue type. [A:aoc_service_name+0x0–0xc; aoc_service_is_queue+0x0–0x8; aoc_service_is_ring+0x0–0xc; aoc_service_is_buffer+0x0–0xc; aoc_service_irq_index+0x0–0x8, text 0x5988–0x59e8.]

There are two region records, each 0x1c bytes, starting S+0x24. **Direction 0 is AOC→AP, direction 1 is AP→AOC**, proved by wrapper arguments, regardless of firmware-centric TX/RX names. [A:aoc_service_read+0x234–0x248, text 0x8f9c–0x8fb0; A:aoc_service_write+0x188–0x19c, text 0x8318–0x832c.]

| Field | Direction 0 | Direction 1 | Queue interpretation |
|---|---|---|---|
| offset | S+0x24 | S+0x40 | Data offset relative to **C**, not carveout start or S. |
| size | S+0x28 | S+0x44 | Slot size including 8-byte transport header. |
| slots | S+0x2c | S+0x48 | Number of slots; zero disables region. |
| tx | S+0x30 | S+0x4c | Monotonic u32 producer count. |
| rx | S+0x34 | S+0x50 | Monotonic u32 consumer count. |
| wp | S+0x38 | S+0x54 | Auxiliary write position; byte position for rings. |
| rp | S+0x3c | S+0x58 | Auxiliary read position; byte position for rings. |

Binary derivations: direction stride/fields A:aoc_service_slots_available_to_read+0x28–0xb8; data-base and modulo arithmetic A:aoc_service_current_read_pointer+0x2c–0x8c and aoc_service_current_write_pointer+0x2c–0x8c; slot overhead A:aoc_service_message_size+0x40–0x54. H names the seven fields; no need to rely on those names for the verified arithmetic.

For queues, occupancy=`(u32)(tx-rx)`, empty when zero, full when occupancy==slots. Slot address=`C+offset+(counter % slots)*size`, using tx to write and rx to read. The first u16 is payload length; next six bytes are zero on Linux writes; payload begins +8. Maximum payload is size-8. [A:aoc_service_slots_available_to_read+0x90–0xb8; aoc_service_can_write_message+0x6c–0x9c; aoc_service_write_message+0x1bc–0x224, text 0x6bb8–0x6c20; aoc_service_read_message+0x1b0–0x21c, text 0x6874–0x68e0.]

Producer commit increments tx by one (u32 wrap), and writes `wp=(old_wp+1)%size`; consumer commit increments rx and writes `rp=(old_rp+1)%size`. **For queues, choose slots with tx/rx modulo slots, not wp/rp.** These auxiliary updates are what this binary does, even though modulo size looks unusual for queue bookkeeping. [A:aoc_service_advance_write_index+0xdc–0x12c, text 0x62a0–0x62f0; aoc_service_advance_read_index+0x9c–0xec, text 0x63dc–0x642c.]

For byte rings (type0x10000), size is byte capacity, tx/rx count bytes, wp/rp select byte positions and data may wrap in two copies; there is no per-slot 8-byte queue framing. Ring writes can overwrite unread data, with overflow recovery on reads. These paths are **not** the TBN queue recipe. [A:aoc_service_write_message+0x15c–0x1b8 and +0x22c–0x28c, text 0x6b58–0x6bb4,0x6c28–0x6c88; A:aoc_service_read_message+0x88–0x1ac, text 0x674c–0x6870.]

### Notification and polling

After writing a queue message, the wrapper sends a **NULL payload** to `mbox_send_message` on the descriptor-selected channel. It does not write service N into a mailbox shared-data register. [A:aoc_service_write+0x1bc–0x1d0, text 0x834c–0x8360; timeout variant +0x258–0x26c, text 0x87c4–0x87d8.]

**Source-based MMIO, exact-build controller unverified:** M:wc_mbox_int_generate writes `1<<local_channel` to controller+0x20. M:wc_mbox_send_data skips shared-register writes for NULL data. RX status is controller+0x4c, clear is controller+0x44 with the same bit. INTMR1 at +0x48 masks AP IRQs (1=masked). [M:functions above, register definitions; source lines 21–31,72–85,122–132,152–166; rendered lines 27–36,77–89,127–132,157–166.]

Derived AP send register for mailbox index I is:

```
B = {0x15200000,0x15220000,0x15240000}[I/16]
write32(B+0x20, 1u << (I%16))
```

[DT lines 13403–13453 plus M cited above; **source-based inference for this build**.]

PCU DT set/clear offsets imply `0x1841f004/0x1841f008`, but **these are not the writes made by the binary service send path**. Probe stores their property values at private +0x484/+0x488; the verified send path calls the external mailbox framework. Do not substitute PCU doorbells for mailbox notification without additional evidence. [A:aoc_platform_probe+0x120–0x18c, text 0x1ff8–0x2064; A:aoc_service_write+0x188–0x1d0.]

Without interrupts, poll **S+0x30 versus S+0x34** for an AOC reply. Poll **S+0x4c minus S+0x50** for outgoing queue space. A mailbox interrupt is a hint to scan queues; message existence is determined by counters. After consuming a previously full incoming queue, Linux sends the same NULL notification to unblock the producer. [A:aoc_service_slots_available_to_read; A:aoc_service_read+0x220–0x264, text 0x8f88–0x8fcc.]

Linux orders payload stores with `dsb st` before producer update; `writel` includes `dmb oshst` before its 32-bit store, and `readl` includes `dmb oshld` after load. Bare-metal integration must preserve visibility/order, including publishing counters before notification. MMU-off alone does not prove absence of stale caches. [A:aoc_service_write_message+0x210–0x224, text 0x6c0c–0x6c20; local readl+0x44–0x48, text 0x6de4–0x6de8; local writel+0x50–0x54, text 0x6e7c–0x6e80. Cache-maintenance requirements for this probe remain unverified.]

## 3. Post-start handshake

No host-ready write, queue reset, service-open packet, or time-sync request appears in the traced core startup→online→TBN-probe path. Online work validates the firmware-populated table and allocates Linux device/waitqueue objects; TBN probe only saves its service-device pointer. **Do not zero the control area, service table, or counters after magic appears.** This is a conclusion about these traced paths, not proof that every optional AOC service has no setup protocol. [A:aoc_did_become_online+0x50–0x3b8; D:aoc_tbn_service_probe+0x0–0x10.]

Linux does deassert `aoc_req` on becoming online: `request_aoc_on(aoc,false)` writes **0 to PA0x154b0000**. This is a power request, not a control-block host-ready flag. Keeping it asserted while testing is an integration choice, not the exact Linux online behavior. [A:aoc_did_become_online+0x58–0x60, text 0x3b60–0x3b68; A:request_aoc_on+0xc–0x18; DT reg-names/reg.]

Clock sysfs access reads C+0x84; it is not a prerequisite transaction on this startup path. [A:clock_offset_show+0x58–0x68; A:aoc_clock_show+0x94–0xa8.]

## 4. com.google.tbn_service protocol

D registers its driver using the service-name list at `.rodata+0x28`, whose relocation points to `.rodata+0xd = "com.google.tbn_service"`. Core matches names, then its probe stores the chosen service-device pointer. There is **no fixed numeric service index in D**. Enumerate the live table and match the full NUL-terminated name. [D:init_module+0xc–0x14 in .init.text; D:.rela.data+0xb0, .rela.rodata+0x28; D:aoc_tbn_service_probe+0x0–0x10; A:driver_matches_service_by_name+0x10–0x34.]

DT `/tbn` has `tbn,mode=2`, the AOC channel branch. `tbn_request_bus_with_result` calls handshaking with operation **2** when the first requester needs the bus. Release of the last requester calls operation **1**. Requester masks/refcounts are Linux-local; they are not in the wire payload. [DT lines 21276–21279; T:tbn_request_bus_with_result+0x34–0x64; T:tbn_release_bus+0x3c–0x50; T:tbn_handshaking+0x64–0xbc.]

Exact request payload (8 bytes, no command-header library required):

| Byte offset | Width | Value |
|---|---|---|
| 0 | u32 | Transaction ID; Linux increments its previous ID before send. |
| 4 | u32 | 2=request bus for AP; 1=release bus to AOC. |

[T:tbn_handshaking+0xd0–0xf8, text 0x1d8–0x200: load ID from packed private +0xad, increment, paired store ID/op, write exactly8 bytes. Direction meaning from the request/release call sites above.]

Example first AP request payload: `01 00 00 00 02 00 00 00`. Its **transport slot** begins `08 00 00 00 00 00 00 00`, followed by that payload. A fresh bare-metal client choosing ID1 is a proposed choice; use a fresh transaction ID and match it, rather than assuming the firmware requires1. [Derived from the verified serialization and A queue header path.]

Expected ordinary reply payload is **13 bytes**:

| Byte offset | Width | Meaning |
|---|---|---|
| 0 | u32 | Echoed transaction ID; must match outstanding request. |
| 4 | u32 | Status; zero is success. Nonzero is recorded as error. |
| 8 | u32 | Operation; 1 completes release, 2 completes request. |
| 12 | u8 | Result byte returned to the caller; semantic bit definitions unresolved. |

[T:aoc_channel_kthread+0x114–0x15c, text 0xd30–0xd78; +0x17c–0x188, text 0xd98–0xda4; +0x2a8–0x2c4, text 0xec4–0xee0; T:tbn_request_bus_with_result+0x68–0x78 copies the result byte out. T:tbn_handshaking+0x294–0x2c4 turns nonzero response status into -EBUSY.]

Reply operations3 and4 are asynchronous paths, not successful ownership replies: op3 schedules reset work; op4 expects22 bytes of gesture data. A poller must consume/dispatch these without treating them as its acknowledgement. [T:aoc_channel_kthread+0x100–0x114, +0x160–0x178, +0x1dc–0x23c, text 0xd1c–0xd30,0xd7c–0xd94,0xdf8–0xe58.]

D write wrapper supplies **200 jiffies**, not proven200ms; the core passes the timeout to `schedule_timeout` without a millisecond conversion. T waits **125 jiffies** for completion and retries failed writes up to three total attempts with the same request buffer. D read uses blocking `aoc_service_read(...,true)`. HZ remains unknown (at HZ250 these are800ms write-space timeout and500ms reply wait). [D:aoc_tbn_service_write+0x18–0x20; D:aoc_tbn_service_read+0x18–0x20; A:aoc_service_write_timeout+0x68, +0x314–0x31c; T:tbn_handshaking+0xf8–0x170, +0x244–0x250.]

G's PM update invokes `tbn_request_bus_with_result` and checks the return value; it adds no IPC framing. [G:goog_pm_state_update_work+0xb0–0xc4, text 0x7468–0x747c.]

Firmware cross-check: `aoc.bin` contains "gti: Change bus owner to AP." at file0x111507 / AOC0x3f5acac7, and the AOC variant at file0x111524 / AOC0x3f5acae4, using the supplied A32 translation. The service name occurs at file0xb671c8 and0xc23bd0 (the latter next to `usf_tbn_server.cc`). **String presence corroborates the feature, not opcodes or reply semantics.** No verified code xref to those logs was recovered; no hardware mux-write claim is made from these strings. Do not interpret the nearby static data as a live IPC descriptor or hardcode an index from it.

## 5. Minimal ordered polled client

This is a derived implementation recipe, **not a tested device result**. Queue framing/counters/payload are binary-verified; the mailbox register step depends on M. Runtime table values were not provided, so literal TBN descriptor/buffer PAs cannot honestly be filled in offline. The formulas below resolve them exactly from AP physical memory.

1. After the already-working start, repeatedly read32(`0x95c00000`) until `0xa0c00a0c`, using a bounded boot timeout. Match Linux's memory-ready predicate, not C+0xc. A 5-second budget is only a suggested policy conditional on HZ250, not a proved firmware deadline. [§1.]
2. Read count at `0x95c00018`, stride at `0x95c0001c`, table offset at `0x95c00020`. Require1..200 services, a stride covering the fields through+0x5b, no address overflow, and all ranges inside the known carveout `0x94000000..0x96ffffff`. Scan `S=C+table_offset+N*stride`, N<count, for `com.google.tbn_service`. These stronger bounds are client-side validation; Linux's helper alone is weaker. [A:service_at_index; aoc_did_become_online; validate_service; arithmetic above.]
3. Require `(read32(S+0x20)&0x70000)==0`. Let I=`(read32(S+0x20)>>19)&31`. Read both queue records. Require nonzero slots, outgoing size>=16 (8 transport+8 request), incoming size>=21 for13-byte replies, and validate entire `C+offset .. C+offset+slots*size` ranges. These size/bounds tests are derived defensive checks. [§2.]
4. Optionally mirror Linux's online power-request release with write32(`0x154b0000`,0); do not overwrite IPC flags or initialize queues. For the first isolated experiment, retaining the existing power request may simplify keeping AOC awake, but that is an unverified integration choice. [§3.]
5. Set `Qd=S+0x40` (down). Wait until `(u32)(read32(Qd+0xc)-read32(Qd+0x10)) < read32(Qd+8)`. Read tx, size, slots. Compute `P=C+read32(Qd)+(tx%slots)*size`. Write exactly:

   ```
   P+0x00: u16 8
   P+0x02: six zero bytes
   P+0x08: u32 transaction_id     // e.g.1
   P+0x0c: u32 2                  // AP ownership
   ```

   Make stores visible (`dsb st` as in Linux), then write32(`Qd+0xc`,tx+1) and write32(`Qd+0x14`,(old_wp+1)%size), preserving u32 wrap. Do not modify down.rx. [A:aoc_service_write_message+0x1bc–0x224; aoc_service_advance_write_index+0xdc–0x12c.]
6. Order the counter writes before notification. **M-based step:** write32(`0x15200020`,1<<I) if I<16; otherwise write32(`0x15220020`,1<<(I-16)). This follows DT ordering and the descriptor's five-bit index. Do not write a service number, do not touch shared mailbox payload registers, and do not use `0x1841f004` as a substitute. [§2 notification evidence and limitation.]
7. Set `Qu=S+0x24` (up). Poll u32 tx at **S+0x30** and rx at **S+0x34** until unequal, with ordered loads. Compute `R=C+read32(Qu)+(rx%slots)*size`. Read u16 length at R, validate length<=size-8 and buffer capacity, then copy payload from R+8. For an ownership reply require13 bytes, matching ID, status0 and op2. Consume unrelated valid asynchronous messages and continue within timeout. Result byte is returned data, not a separately verified ownership bit. [A:aoc_service_read_message+0x1b0–0x218; T:aoc_channel_kthread cited above.]
8. After copying each consumed payload, write32(`Qu+0x10`,rx+1) and write32(`Qu+0x18`,(old_rp+1)%size). Do not modify up.tx. If incoming queue was full before the consume, notify on the same mailbox bit, matching Linux's unblock behavior. For mailbox housekeeping, M permits polling B+0x4c and clearing observed bits via B+0x44; queue counters remain authoritative. No GIC setup is needed solely for this polled approach. [A:aoc_service_advance_read_index+0x9c–0xec; aoc_service_read+0x220–0x264; M:wc_mbox_peek_data/wc_mbox_int_clear.]
9. To return the bus, repeat with a new ID and operation1; require matching13-byte reply, status0/op1. Do not infer success merely because outgoing tx was consumed or the mailbox bit cleared. [T:tbn_release_bus+0x4c–0x50; T:aoc_channel_kthread+0x134–0x188.]

### Remaining uncertainties

* Exact-build mailbox controller binary was absent; M provides a concrete, compatible source recipe but cannot prove this firmware/kernel combination has no controller changes.
* Live TBN N, I, offsets, slots and slot sizes require reading the published table. The four observed startup words do not contain them.
* No result-byte bit meanings, firmware mux-register sequence, or cold-boot cache/access-control requirements were proved.
* No further host-ready handshake was found in the analyzed path; this is not a claim about all firmware services or all versions.
