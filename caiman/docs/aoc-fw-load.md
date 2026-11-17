# AOC firmware loading: static findings for caiman

Read-only analysis of the supplied ELF relocatables, disassembly, firmware and live DT; no device access or execution. Addresses below are **module-relative .text offsets**, not loaded addresses. `A:` means aoc_core.ko, `S:` means samsung_iommu_v9.ko. A citation such as `A:aoc_fw_callback @0x314` identifies the instruction at absolute .text offset 0x314 within that named function. DT and file-byte observations are cited as such; computed results are labelled derived. No claim here establishes runtime accessibility of MMIO or successful hardware startup.

Input SHA-256:

- aoc_core.ko: `1a348d8c60b0f70363fb66a8b255c9ea320e8144781ec819c4a971eb1ef9ad50`
- samsung_iommu_v9.ko: `baff197b163c02f69572c5aa3a7e4de9a16475cd5e5d3b25ce4429ff9145c911`
- aoc.bin: `cba2423541a1b2467f8324dfdcace38804e5404970a65aecc6dad9b53eab9c96`
- caiman-live.dts: `9642aace442abd6820732077eda51f26322a7efadb063f66ef7e7d59eaec5f2c`

## Important corrections to aoc-boot-map.md

The actual callback **does not call aoc_configure_iommu on the GSA path**. It tests presence of `gsa-enabled` (.rodata+0x5e4 relocation at 0x898); non-NULL branches from 0x8b0 to 0x92c, calling the fault-handler setup then authentication. The mapping and reset-trampoline calls at 0x8bc/0x8c4 belong to the non-GSA branch. Similarly 0x9a4 branches around `aoc_release_from_reset` when that property is present, instead calling `gsa_send_aoc_cmd(1)` at 0x9bc. Thus reproducing these seven mappings manually is a proposed bare-metal construction, not the observed GSA-enabled Linux callback sequence. Who establishes translations during GSA authentication is **unresolved** in these inputs. [A:aoc_fw_callback @0x894–0x8c8, 0x92c–0x960, 0x9a4–0x9bc; DT aoc@17000000 `gsa-enabled`.]

There are 22 entries, but the key set is **0x1001, 0x1002, 0x1003, 0x1005…0x1017**; 0x1004 is absent. [A:aoc_fw_callback @0x410–0x530; A:aoc_pass_fw_information @0x1a78–0x1ad4.]

## A. Firmware information

The serializer writes words `{0x0a0cda7a,22,0x114}` followed immediately at +0xc by 22 `{key,4,value}` triples (32-bit words, 12-byte stride). Total length is 0x114. The callback supplies an array of 8-byte `{key,value}` pairs at sp+0x30. IPC destination is mapped carveout+firmware IPC offset, hence derived PA 0x95c00000 for base 0x94000000 and offset 0x1c00000. [A:aoc_pass_fw_information @0x1a64–0x1ad4; A:aoc_fw_callback @0x944–0x960; A:_aoc_fw_ipc_offset @0x75f8; firmware word +0x100c=0x1c00000.]

The following table is in **serialization order**, after board fixups. Zero tunables mean fresh-probe defaults, not a measurement of an Android process's later ioctl state. `R`, `REV`, `TYPE`, `PRODUCT` are unresolved values, not zero placeholders.

| Key | Concrete value / expression | Source and instruction evidence |
|---|---|---|
| 0x1001 | 0x00040902 | Initial constant 0x20202 at callback 0x338–0x35c, initial pair at 0x45c; DT `aoc-board-id` read at 0x5c0, replaced by fixup loop 0x674–0x6bc. |
| 0x1002 | 0x00010000 | Initial constant 0x10000 at 0x34c/0x358, pair at 0x46c; DT `aoc-board-rev` read 0x60c, fixup 0x674–0x6bc. |
| 0x1003 | 0 | Zero high word of x15 when stored as pair at callback 0x49c. No independent DT source. |
| 0x1005 | 0x94000000 | Low 32 bits aoc+0x50: load 0x300, save 0x308, pair 0x490. |
| 0x1006 | 0x03000000 | Low 32 bits aoc+0x188: load 0x30c, save 0x310, pair 0x484. |
| 0x1007 | 0x9ac00000 | aoc+0x1b8 heap PA translated to AOC address at 0x410–0x44c; pair 0x4ac. |
| 0x1008 | 0x00400000 | Constant heap length, callback 0x488–0x4a0. |
| 0x100c | 0x9abfc000 | aoc+0x1c0 heap PA translated at 0x444–0x464; pair 0x4bc. |
| 0x100d | 0x00004000 | Constant, callback 0x4a4–0x4c0. |
| 0x100e | 0x9abec000 | aoc+0x1c8 heap PA translated at 0x468–0x4b8; pair 0x4d0. |
| 0x100f | 0x00010000 | Constant, callback 0x4c4–0x4d8. |
| 0x1009 | 0 | DT `force-vnom`=0 plus aoc+0x388=0; computation 0x2f0–0x330, pair 0x4e8. |
| 0x100a | 0 | aoc+0x380, load 0x344, pair 0x4ec. |
| 0x100b | 0 | aoc+0x384, load 0x334, pair 0x4f4. |
| 0x1010 | 0 (fresh probe) | aoc+0x390, load 0x350, pair 0x500. |
| 0x1011 | R | `get_random_u32()` at 0x35c; pair 0x508. |
| 0x1012 | REV | `gs_chipid_get_revision()` at 0x364; pair 0x510. |
| 0x1013 | TYPE | `gs_chipid_get_type()` at 0x36c; pair 0x518. |
| 0x1014 | 2 | DT `gnss-type` read 0x39c; default/error handling 0x3c4–0x3d0; pair 0x520. |
| 0x1015 | 0 (fresh probe) | aoc+0x394, load 0x340, pair 0x528. |
| 0x1016 | PRODUCT | `gs_chipid_get_product_id()` at 0x374; pair 0x530. |
| 0x1017 | 0 | `wifi-chip` absent in supplied AOC DT node; read/default 0x3d4–0x408; pair 0x420. |

All instruction sites in the table are A:aoc_fw_callback. Relocation-resolved property names: .rodata+0x8f8=`force-vnom` (0x2f0), +0x1562=`gnss-type` (0x380), +0x903=`wifi-chip` (0x3d4); board configuration reads are callback 0x568 (`aoc-board-cfg`, DT `CM4`), 0x5c0 (`aoc-board-id`, DT 0x40902), 0x60c (`aoc-board-rev`, DT 0x10000). The loop at 0x674–0x6bc searches existing keys and replaces their values; CM4 is not a 23rd serialized entry. [A:aoc_fw_callback @0x534–0x6d0; DT aoc@17000000 lines 13439 onward.]

For 0x1009 the general expression is initially 1; it becomes boolean(aoc+0x388) only if reading `force-vnom` succeeds and returns zero. Missing/error/nonzero DT therefore yields 1. [A:aoc_fw_callback @0x2e4, 0x314–0x330.]

Chip-ID functions and RNG are imports: the module proves the sources, not returned values. Product naming alone is insufficient evidence to fill those words. Heap AOC addresses in the table are derived from §B and the actual reserved-memory range. [A:aoc_fw_callback @0x35c–0x374, 0x410–0x4b8.]

## B. aoc fields and SRAM

| Field | Assignment and meaning |
|---|---|
| +0x50 / +0x58 | Embedded resource start/end inclusive. `memory-region` phandle parsed at probe 0x1f24–0x1f54; `of_address_to_resource(node,0,aoc+0x50)` at 0x22f0–0x2304. DT phandle0x1ff gives start0x94000000, end0x96ffffff. |
| +0x150 | Linux `struct device *` = platform-device+0x10, stored probe 0x2190 (x19 initialized 0x1f08). Not an AOC hardware address. |
| +0x160 | IPC virtual address, assigned callback 0x988–0x9a0 as +0x170+ipc_offset, with NULL/bounds checks. Derived bare-metal PA0x95c00000. |
| +0x170 | DRAM carveout CPU mapping returned by `devm_ioremap_wc(start,size)` at probe 0x2518–0x2538, stored at 0x2594–0x259c. |
| +0x188 | Resource byte length=end-start+1, stored probe 0x24e0–0x24f8; 0x3000000 here. |
| +0x1b8 | sensor_direct_heap PA=end-0x3fffff=0x96c00000, set `aoc_create_dma_buf_heaps` 0x1050–0x1058/0x10d0; size0x400000 at0x1078. |
| +0x1c0 | aaudio_playback_heap PA=end-0x403fff=0x96bfc000, set helper 0x1094–0x10a8/0x1114; size0x4000 at0x10c0. |
| +0x1c8 | aaudio_capture_heap PA=end-0x413fff=0x96bec000, set helper 0x1094–0x109c/0x1130; size0x10000 at0x1104. |
| +0x380 | Monitor-mode-disabled flag; explicitly zeroed probe0x2194; ioctl writes0x4244, message .rodata+0x190a. |
| +0x384 | UART logging enabled; zeroed probe0x2194; ioctl write0x449c, message .rodata+0x1d8d. |
| +0x388 | Force nominal voltage; zeroed probe0x2198; ioctl write0x4664, message .rodata+0x2e2. |
| +0x38c | AP-side resets disabled; zeroed probe0x2198; ioctl write0x4580, message .rodata+0x61a; consumed callback0x7ec–0x800. |
| +0x390 | Force speaker ultrasonic pipeline; ioctl write0x4370, message .rodata+0xcfe. No explicit probe store found; fresh zero derives from allocation flags below. |
| +0x394 | Release MIF on VoLTE; ioctl write0x4748, message .rodata+0x23 referenced0x4834. Same allocation-default qualification. |
| +0x3c0 | GSA Linux device pointer: parse `gsa-device` (.rodata+0x134e) at probe0x21b0–0x21e0, find device0x21f0, add0x10 and store0x220c. DT phandle0x6e. |
| +0x47c | DT `pcu-base` (.rodata+0x3a4), read probe0x1f84–0x1fa4, stored0x1fb8; DT0x1400000. Missing read stores -1 and fails probe. |

Table sites are A:aoc_platform_probe unless another function is named; ioctl is A:aoc_unlocked_ioctl. Heap helper is called at probe0x25e0; heap names resolve from .rodata+0x17a0, +0xf36, +0x1150 at helper0x105c/0x10a0/0x10e8. The fresh allocation is `devm_kmalloc(dev,0x4b0,0xdc0)` at probe0x1f60–0x1f70. Interpreting 0x100 within those flags as `__GFP_ZERO` is a Linux ABI interpretation; zero defaults for +0x390/+0x394 depend on it, not explicit stores. User ioctl changes can alter every tunable listed. [A:aoc_unlocked_ioctl @0x4244,0x4370,0x449c,0x4580,0x4664,0x4748.]

`aoc_dram_translate_to_aoc(pa)` returns `(uint32_t)(pa-start+0x98000000)` if `start <= pa < end+1`, else zero. This is distinct from SRAM translation. [A:aoc_dram_translate_to_aoc @0x6c–0x8c; inlined equivalents A:aoc_fw_callback @0x410–0x4b8.]

`aoc_sram_translate(arg)` takes an **offset**, zero-extends its low32 bits, and returns `sram_mapping+offset` when `offset <= resource.end-resource.start+1`, otherwise NULL; absent mapping triggers BRK. The exact comparison admits offset==length (an apparent boundary bug, not a recommended bound). `blk_aoc` resource obtained probe0x22d8–0x22fc (.rodata+0x1d62), mapped0x24d4–0x24ec, and DT has PA0x17000000 length0x2000000. [A:aoc_sram_translate @0x4–0x38.]

`aoc_release_from_reset` reads aoc+0x47c, translates it, reads a 32-bit word, ORs bit0, and writes it back. Derived target is **0x17000000+0x1400000=0x18400000**, value `old|1`, not an unconditional write of 1. It returns whether translation was non-NULL. This callback only calls it on the non-GSA branch. [A:aoc_release_from_reset @0x9210–0x9238; A:aoc_fw_callback @0x9a4–0x9bc.]

## C. SysMMU v9 translation programming

DT `/sysmmu@19090000` (label sysmmu_aoc, lines1314–1326) supplies nonsecure base B=0x19090000 length0x10000, QoS15, secure-base0x190a0000, IRQ SPIs0x84/0x85, `sysmmu,secure-irq`, `sysmmu,leave-enabled-on-suspend`, pmmu0 default0x10b and stream tuples below. The driver defaults absent vmid_mask to1, selecting VID0. [S:samsung_sysmmu_device_probe @0x7ac; DT cited node.]

### Register writes verified in enable

All offsets below are relative to B; v is VID. These are operational descriptions from accesses, not independently verified hardware register names.

| Offset | Operation / role | Evidence S:__sysmmu_enable |
|---|---|---|
| 0x8404+v*0x1000 | FLPT physical address >>12, low32 written | 0x3178–0x31c4 |
| 0x8408+v*0x1000 | Context configuration RMW, formula below | 0x31d0–0x3248 |
| 0x2ffc | Select PMMU index&15, then read back same address | 0x3284–0x32b4 |
| 0x2000 | Selected PMMU default-stream configuration, mask0xffff017f | 0x32b8–0x32d0 |
| 0x2000+16*i | Stream i config &0xffff017f | 0x32f0–0x3314 |
| 0x2004+16*i | Stream match config &0x301 | 0x3318–0x332c |
| 0x2008+16*i | Stream match ID | 0x3330–0x3340 |
| 0x200c+16*i | Stream match mask | 0x3344–0x3354 |
| 0x8000+v*0x1000 | Read, OR1, write: context enable | 0x3380–0x33cc |
| 0x8010+v*0x1000 | Write1: invalidate all | 0x33d8–0x341c |

Context config calculation is `t=(old & 0xfff0fffb)|0x00020004`; when QoS!=-1, `t=(t & 0xfff2f07f)|((qos&15)<<7)|0x800`. With DT qos15 this ORs0xf80 after masking. Preserve the masked old bits; an unconditional literal has not been justified. The driver writes PT bases first, then context configs, then PMMU/default/stream settings, then enables contexts, then invalidates. [S:__sysmmu_enable @0x3154–0x3248,0x324c–0x341c.]

A further enable loop handles nonzero auxiliary context PT pointers for VID1..7 at offsets0x9000..0xf000, using the same +0x404/+0x408/+0x10 layout. Do not populate those contexts merely because the loop exists; the branch skips NULL pointers. [S:__sysmmu_enable @0x3420–0x34cc.]

### PMMU0 streams

Each DT tuple is five u32s `{index,config,match_config,match_id,mask}`. The enable loop skips index0xffffffff; default0xffffffff skips its write. Here select PMMU0, default config0x10b. For every tuple below match_config=0x301 and mask=0x3f; config=0x10b except index1 config=0x0100010b. These survive the driver's config masks unchanged. [S:__sysmmu_enable @0x3284–0x3354; DT pmmu0 properties.]

| Index | Match ID | Index | Match ID | Index | Match ID |
|---|---|---|---|---|---|
|1|0x02|11|0x0e|21|0x30|
|2|0x03|12|0x20|22|0x31|
|3|0x04|13|0x21|23|0x32|
|4|0x05|14|0x22|24|0x33|
|5|0x06|15|0x23|25|0x34|
|6|0x09|16|0x24|26|0x35|
|7|0x0a|17|0x25|27|0x36|
|8|0x0b|18|0x26|28|0x37|
|9|0x0c|19|0x27|29|0x38|
|10|0x0d|20|0x28|—|—|

These are raw register encodings; assigning semantic names to all config bits would be a guess. [S:__sysmmu_enable @0x330c,0x3324–0x3354.]

### Page tables and a concrete construction

The module uses a two-level format of 32-bit descriptors, not AArch64 stage-1 translation tables. FLPT allocation is0x40000 bytes aligned0x40000; SLPT allocation0x400 bytes aligned0x400. FLPT index=`iova>>20`; SLPT index=`(iova>>12)&255`. A first-level table descriptor is `(SLPT_PA>>4)|1`; its decoded address is `(entry&0xffffffc0)<<4`. Section type low3=2 covers1MiB. Second-level type low2=2 covers4KiB, type1 covers64KiB with16 repeated entries. For prot3 and no DT AP overrides, section descriptor=`(PA>>4)|0x32`, 4KiB descriptor=`(PA>>4)|0xe`, 64KiB descriptor=`(PA>>4)|0xd`. [S:samsung_sysmmu_init_global @0xc58–0xc90; S:samsung_sysmmu_map @0x2268–0x2384,0x24cc–0x25d0,0x25f4–0x2724; S:samsung_sysmmu_iova_to_phys @0x3014–0x309c.]

**Derived proposed page-table layout**, equivalent to the seven firmware mappings, not a dump of Linux's allocated tables: zero the FLPT and one SLPT; install 56 section descriptors for the first five rows, and FLPT[0x9e3]=(SLPT_PA>>4)|1 for the last two rows. In that SLPT, entries0..127 map PA0x40000000+j*0x1000; entries128..255 map PA0x40400000+(j-128)*0x1000; encode each as `(PA>>4)|0xe`. This needs0x40000+0x400 bytes of tables at appropriately aligned, accessible physical storage. [Derived using descriptor rules above and A:aoc_configure_iommu @0x18ac–0x18cc; aoc.bin image-config table, prior map §9.]

| IOVA | PA | Length | Derived FLPT indices |
|---|---|---|---|
|0x98000000|0x94000000|0x3000000|0x980..0x9af|
|0x9b000000|0x97000000|0x400000|0x9b0..0x9b3|
|0x9e000000|0x15100000|0x200000|0x9e0..0x9e1|
|0x9e200000|0x16400000|0x100000|0x9e2|
|0x9e500000|0x11200000|0x100000|0x9e5|
|0x9e300000|0x40000000|0x80000|0x9e3 L2[0..127]|
|0x9e380000|0x40400000|0x80000|0x9e3 L2[128..255]|

### Discovery, attach, synchronization and limits

Probe maps resource0 into driver+0x50 at0x3ec–0x410; reads version from `(B+0x34)>>16` at0x4e4–0x4fc, VID count from `read(B+0x100)&0x1f` at0x500–0x514, and PMMU count from `read(B+0x3004)&0xffff` at0x518–0x528. Context type comes from low5 bits at B+0x140+4*VID; type1 is accepted, type0 fails, other types warn. PMMU count and stream capacity are hardware-dependent, not proved by DT having only pmmu0 properties. [S:samsung_sysmmu_device_probe @0x530–0x5a4.]

Property-name relocations resolve .rodata+0x11f8=`pmmu%d,default_stream` and +0x59a=`pmmu%d,stream_property`. Probe counts records with element size0x14 and reads count*5 words; it selects each PMMU with B+0x2ffc write/readback, reads B+0x3000>>16 for maximum stream count, and marks an out-of-range record index -1. A bare-metal implementation should preserve this capacity check instead of assuming all29 entries are accepted. [S:samsung_sysmmu_device_probe @0x828–0x960.]

Absent `sysmmu,ap-read-implies-write` and `sysmmu,ap-permissive` properties produce zero flags in driver+0xcc/+0xcd, then domain+0x84/+0x85 on attach; this is the basis for the prot3 descriptor encodings above. [S:samsung_sysmmu_device_probe @0x738–0x794, .rodata+0x37f/+0x9c3; S:samsung_sysmmu_attach_dev @0x1da4–0x1dc0; DT sysmmu@19090000.]

The attach path converts the domain FLPT allocation to PA at0x1a44–0x1a80, stores it in driver+0x60 at0x1b40–0x1b48, and calls enable on first attachment when runtime state permits at0x1b04–0x1b64. Thus Linux can enable an initially empty table before later mapping calls. Domain allocation creates the FLPT and bookkeeping at0xe40–0xe68 and initially DMA-syncs it at0xeb0. [S:samsung_sysmmu_attach_dev and S:samsung_sysmmu_domain_alloc at those sites.]

The mapper DMA-syncs newly allocated SLPT0x400 bytes at0x24a0–0x24f4, a changed FLPT word at0x2520–0x2530, and section descriptors at0x237c–0x23ac; mapped L2 ranges are synchronized in iotlb_sync_map. These are Linux visibility operations, not evidence that an EL2 MMU-off implementation needs no cache maintenance. [S:samsung_sysmmu_map at those sites; S:samsung_sysmmu_iotlb_sync_map @0x2d88–0x2df8.]

Invalidate-all also appears in flush_iotlb_all at B+0x8010+VID*0x1000=1. Range invalidation writes B+0x8020+VID*0x1000=`(start>>4)&0xffffff00`, then B+0x8024+VID*0x1000=`((end>>4)&0xffffff00)|1`. No completion polling is present in these routines. [S:samsung_sysmmu_flush_iotlb_all @0x2c30–0x2c4c,0x2cd8–0x2d24; S:samsung_sysmmu_iotlb_sync @0x2e74–0x2eb8,0x2f50–0x2ff8.]

Disable clears CTRL bit0, clears PT base and invalidates additional populated contexts at0x3510–0x3578; it clears bit0 and invalidates normal enabled VIDs at0x357c–0x3624. This is evidence for disabling existing translation, not authorization to discard an unknown inherited hardware configuration. [S:__sysmmu_disable at those sites.]

### Secure boundary

Probe reads `sysmmu,secure_base` (.rodata+0xc8a) into driver+0xc4 at0x690–0x6a8 after secure-IRQ handling0x628–0x688. No secure-base write or SMC occurs in attach0x1940–0x1e0c or enable0x312c–0x34f0. The module's SMC relocations instead occur in fault handling: sysmmu_get_interrupt_info0x39c4/0x3aa8/0x3afc, sysmmu_show_secure_fault_information0x3c6c–0x40fc, sysmmu_clear_interrupt0x504c/0x50f4, sysmmu_get_fault_msg0x5260. Secure reads use function0x820020d0 and secure_base+offset; clear at0x50f4 uses0x820020d7 with secure_base+(VID<<12) and status. [S:those functions and sites.]

Probe calls external `pkvm_s2mpu_of_link_v9` at0x3c0. Its effects, access permissions, clocks/power prerequisites, and any GSA-side secure translation setup are outside the supplied implementation. Therefore these register/table findings are **not proof of a sufficient, directly accessible bare-metal initialization sequence**. No speculative writes to0x190a0000 are prescribed. [S:samsung_sysmmu_device_probe @0x3c0; A:aoc_fw_callback @0x8b0,0x93c.]

## D. Power mode and crash interrupts

`disable_power_mode` and `enable_power_mode` are undefined imports in aoc_core.ko, not functions implemented there. Callback calls them with `(0,1)` at0x97c–0x984 and0xa1c–0xa24, bracketing start plus a2000ms sleep at0xa04–0xa08. Their register addresses, masks and implementation are **unresolved** from the supplied modules; do not invent a PMU sequence from their names. [A ELF symbol table undefined entries; A:aoc_fw_callback at cited sites.]

`configure_crash_interrupts(aoc,enable)` has no direct MMIO accesses. If aoc+0x210 is set, it enables IRQ at+0x1d8 and clears that byte, irrespective of enable. Otherwise enable bit0 selects enable_irq vs disable_irq_nosync for IRQ fields+0x3a0,+0x3a4,+0x1d8. GIC register programming happens behind external Linux IRQ helpers and cannot be derived here. [A:configure_crash_interrupts @0x9b38–0x9b80.]

The first two IRQ fields come from SysMMU DT interrupt indices0/1 (`of_irq_get`,0x9ab0/0x9ac8, stores0x9ac4/0x9ad4); watchdog from `platform_get_irq_byname` and store0x99c0/0x99c4. DT SPI numbers are0x84/0x85 and0x82 respectively; these are DT SPI numbers, not Linux virtual IRQ numbers. Watchdog setup sets+0x210=1 at0x9a08–0x9a0c. [A:configure_iommu_interrupts and configure_watchdog_interrupt at those sites; DT sysmmu@19090000 and aoc@17000000.]

## E. Ordered bare-metal preparation, stopping before GSA command0x5a

This distinguishes the verified Linux GSA ordering from optional manual SysMMU work. It is a static checklist, not a claim of a working loader.

1. Resolve the fields and 22 boot-info values above, retaining unknown chip-ID and fresh random values as inputs still needed. Respect the memory-region range0x94000000..0x96ffffff and distinguish CPU PA, AOC address and Linux pointers. [A:aoc_platform_probe @0x2304,0x24e0–0x24f8; A:aoc_fw_callback @0x300–0x530.]
2. Request AOC on before validation, as callback0x6dc does: request_aoc_on writes `(argument & 1)` to aoc+0x178, the mapping of DT `aoc_req` at PA0x154b0000. With argument1 this is a direct 32-bit write of1, not an RMW. This is separate from the imported power-mode routines. [A:aoc_fw_callback @0x6d4–0x6dc; A:request_aoc_on @0x925c–0x9268; A:aoc_platform_probe @0x2530–0x2558; DT aoc@17000000 reg-names/reg.]
3. Validate image/header and IPC offset before copying. Commit `aoc.bin+0x1000` to carveout, length `file_size-0x1000`, preserving the original first0x1000 bytes separately for authentication. For this file size29076160, copy length is29072064. [A:aoc_fw_callback @0x6f8–0x82c,0x87c; A:_aoc_fw_commit @0x76c4; file-size observation.]
4. **If establishing the mappings manually is an independent requirement**, build the tables in §C, make table contents visible to the coprocessor/page-table walker, and use the enable ordering there: PT base, config RMW, PMMU streams, CTRL bit0, invalidate-all. Physical storage selection and cache maintenance/barriers for the EL2 MMU-off environment are **integration work not established by this static analysis**. Linux GSA-enabled callback skips its ordinary map helper; do not present manual mapping as an unconditional step Linux performs before0x5a. [S:__sysmmu_enable @0x3178–0x341c; A:aoc_fw_callback @0x8b0–0x8c8.]
5. Set up fault reporting if desired; the GSA path registers an IOMMU fault handler before authentication. Linux IRQ registration is not itself a bare-metal MMIO recipe. [A:aoc_fw_callback @0x92c–0x930; A:aoc_configure_iommu_fault_handler @0x169c.]
6. Prepare a physically accessible0x1000-byte header buffer containing exactly aoc.bin[0:0x1000]. Linux allocates it with the GSA device, copies0x1000 bytes, and passes header DMA address plus carveout PA to gsa_load_aoc_fw_image. A bare-metal CPU PA is usable as that argument only if the GSA DMA address relationship is established separately. [A:aoc_fw_authenticate @0x1718–0x175c.]
7. **Stop before invoking gsa_load_aoc_fw_image / mailbox0x5a.** The fw-info serialization, disable_power_mode, and start/reset actions occur after authentication returns successfully, so do not move those writes into the verified pre-0x5a sequence. The table can be prepared in RAM beforehand. [A:aoc_fw_callback @0x93c–0x984,0x9a4–0x9bc.]

Remaining gaps: external power-mode implementation; actual chip-ID words; concrete GSA DMA-address constraints; GSA's own mapping/security effects; clock, access-control and coherency prerequisites for a cold bare-metal SysMMU setup. These are explicitly unresolved, not guessed register writes.
