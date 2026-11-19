# Caiman HSI2 S2MPU v9

Static analysis of the supplied modules, PCIe source and DTS. No device access. Instruction citations are **function-relative hexadecimal byte offsets**. Hyp symbols below omit the ELF prefix `__kvm_nvhe_`. MMIO offsets are relative to the S2MPU base.

## Instance, PCIe binding and VID

`caiman-live.dts:5516–5522` gives base `0x131f0000`, size `0x10000`, compatible `google,s2mpu-v9`, and `always-on`. The host maps resource 0 and checks that property (`s2mpu_probe+0x78–0xb0/+0xd4–0x100` with `.rodata+0x1b1`). The host suspend wrapper skips always-on devices (`__pkvm_s2mpu_suspend+0xc–0x1c`). The hyp validator requires the 64 KiB aperture (`s2mpu_validate+0x0–0xc`).

The supplied RC1 node at `caiman-live.dts:8422` has channel 1, a Broadcom WLAN child, and `use-sysmmu = "false"`. It has no `s2mpu` property. The reference PCIe source binds an S2MPU only under `CONFIG_GS_S2MPU` with that phandle (`pcie-exynos-rc.c:5196–5216`). Its refcount path rounds outward to 4 KiB and calls `s2mpu_open(info, page, 0x1000, dir)` on 0→1 and `s2mpu_close` on 1→0, under a lock. The first open supplies the direction for that page while references remain (`pcie-exynos-rc.c:99–100,119–123,149–221`).

The shipped PCIe module has no S2MPU open or close symbols or call sites. Its DMA wrappers show generic DMA calls and conditional PCIe IOMMU calls (`pcie_dma_alloc_attrs+0xd4/+0x138`, `pcie_dma_free_attrs+0x4c/+0xa0`, `pcie_dma_map_page+0x4c/+0xb4`, `pcie_dma_unmap_page+0x4c/+0xa0`). The source-level open cannot be tied to the shipped hyp callback through these artifacts. **RC1's transaction VID is unresolved.** The source call carries no VID argument and there is no evidence tying VID to `ch-num`. Hyp initializes and updates all eight VID banks (`init_with_mpt+0x44–0x84`, `apply_range+0x48–0x80`).

## Register map

| Offset | Role and fields | Instruction evidence |
|---|---|---|
| `0x0000` | CTRL0 candidate. Both hyp resume and suspend write zero. Enable and fault response bits are unresolved. | `s2mpu_resume+0x68–0x6c`, `s2mpu_suspend+0x64–0x68` |
| `0x0010` | CTRL1 candidate. Both paths write zero. Field meanings are unresolved. | `s2mpu_resume+0x7c–0x84`, `s2mpu_suspend+0x78–0x80` |
| `0x0020` | Written with `0xff`. Per-VID interrupt enable is a **guess** based on the eight-bit mask and nearby fault acknowledgment. | `s2mpu_resume+0x5c–0x64`, `s2mpu_suspend+0x58–0x60` |
| `0x002c` | Fault interrupt acknowledgment. Handler writes `1 << VID`, then rereads fault status. Protected host writes are masked to [7:0]. | `s2mpu_fault_handler+0x280–0x2a4`, `s2mpu_host_dabt_handler+0x28–0x2c/+0x8c–0x94/+0xa8–0xbc` |
| `0x0050` | Written with `0xff`. CFG is a **guess**. Fields unresolved. | `s2mpu_resume+0x70–0x78`, `s2mpu_suspend+0x6c–0x74` |
| `0x0054` | Additional write site on the host resume wrapper's non-hyp branch. Semantics unresolved. | `__pkvm_s2mpu_resume+0x24–0x2c` |
| `0x0068` | STATUS. Invalidation completion loops until at least one of bits [1:0] is set, for cached selectors `0x2000` and `0x9000`. Individual bit meanings unresolved. | `__all_invalidation+0xb4–0xe8`, `s2mpu_host_stage2_idmap_complete+0x5c–0x90` |
| `0x0070` | Written with `0xff`. Register name and fields unresolved. | `s2mpu_resume+0x48–0x58`, `s2mpu_suspend+0x48–0x54` |
| Unresolved | Main VERSION/INFO. The two selector comparisons load a halfword from **software object** offset `0x64`. The MMIO base comes from object offset `0x50`. There is no verified main VERSION offset here. | `__all_invalidation+0xb4–0xe0`, `s2mpu_host_stage2_idmap_complete+0x5c–0x88` |
| `0x0100` | NUM_CONTEXT. Uses bits [3:0], caps count at eight, and rejects zero without a cached context word. | `__initialize_ctx+0x0–0x30/+0x90–0xac/+0x180–0x1a0` |
| `0x0104` | CONTEXT_CFG_VALID_VID. Eight nibbles, one per context, with valid at bit 3 and VID at [2:0] within each. For `n = min(NUM_CONTEXT & 15, 8)`, emits the low `n` nibbles of `0xfedcba98`, zero elsewhere. One context gives `0x8`, two give `0x98`. | `__initialize_ctx+0x34–0x8c/+0xb0–0x194`, constants at `.hyp.rodata+0x108–0x17f` |
| `0x1000` | ALL_INVALIDATION, write 1, then sync. | `__all_invalidation+0x14–0x2c/+0x40–0x50/+0x88–0xb0` |
| `0x1020` | RANGE_INVALIDATION, write 1 after setting bounds. | `s2mpu_host_stage2_idmap_apply+0x94–0xa4` |
| `0x1024`, `0x1028` | Range start and inclusive end, both in 4 KiB PFNs. | `s2mpu_host_stage2_idmap_apply+0x6c–0x90` |
| `0x102c`, `0x1030` | Built-in sync command and status. Write 1 to command and poll status bit 0 for set, with bounded retries. Child sync blocks use offsets `0x0` and `0x4` from their own bases. | `__all_invalidation+0x40–0x50/+0x70–0xb0`, `s2mpu_host_stage2_idmap_apply+0xa8–0xe4`, `__invalidation_barrier_slow+0x0–0x140` |
| `0x2000` | Fault status, low eight bits are a pending VID mask in the handler. Uses `rbit; clz` to select the first set VID. | `s2mpu_fault_handler+0x48–0x90/+0x29c–0x2b0` |
| `0x2004+0x20*v`, `0x2008+0x20*v` | Fault PA low and high for VID `v`. High is read first. The handler assembles a 64-bit PA from the two words. | `s2mpu_fault_handler+0x8c–0xcc` |
| `0x2010+0x20*v` | FAULT_INFO. Bit 20 is direction (0 read, 1 write), [23:21] type, [26:24] VID and [19:16] request length field. The length field is logged as extracted. | `s2mpu_fault_handler+0xb8–0xd0/+0x10c–0x1c0`, strings `.rodata+0x6f/+0x1ea/+0x4ed` |
| `0x2014+0x20*v`, `0x2018+0x20*v` | First is printed as REQ_AXI_ID. Second contains PMMU_ID in [31:24] and STREAM_ID in [23:0]. | `s2mpu_fault_handler+0xd4–0x108/+0x190–0x1d8`, strings `.rodata+0x6f/+0x16` |
| `0x3100`, `0x3104`, `0x3800+4*i`, `0x3c00` | Debug capability reads. PTLB count from `0x3100[15:1]`, STLB count from `0x3104[31:16]`; STLB ways/sets from the high/low half of `0x3800+4*i`; MPT cache set-loop bound from `0x3c00[31:16]` when that dump is enabled. | `s2mpu_fault_handler+0x338–0x354/+0x390–0x3bc/+0x40c–0x434/+0x5fc–0x630/+0x83c–0x874` |

FAULT_INFO type **1** maps to `MPTW fault`, **2** to access permission fault, **3** to unknown fault, and **4** to context fault. Other values use the default unknown string. **Interpretation:** MPTW means a memory protection table walk fault. An inaccessible or invalid SMPT backing address is a plausible cause. The driver provides no subcause or failing-level decode, and the meaning of the recorded PA specifically for a walk error is unresolved. Evidence is `s2mpu_fault_handler+0x14c–0x1a0` and the ABS64 string relocations at `.rodata+0xbc8–0xbe0`.

## L1 FMPT and L2 SMPT

Let `g = PA >> 30` and `v` be VID 0–7. The initialization iterates `g = 0…3, 34…63`, with a jump from 3 to 34. These loops supply no policy for the skipped indices.

| Register | Offset | Encoding |
|---|---|---|
| L2 SMPT address | `0x4000 + 0x200*v + 8*g` | A 32-bit store of `table_pa >> 4`. The software requires 128 KiB alignment for its supplied table allocations. |
| L1 FMPT attribute | `0x4004 + 0x200*v + 8*g` | Uniform 1 GiB mode has bit 0 clear and permission in [2:1]. Values are `0x0` deny, `0x2` read, `0x4` write, `0x6` read/write. Bit 0 set selects SMPT mode. Hyp writes `0x1` for 4 KiB. The fault logger prints 64 KiB when bit 3 is set in SMPT mode. |

Evidence is `init_with_prot+0x0–0x140`, `init_with_mpt+0x44–0xd8`, `s2mpu_init+0x5c–0x88/+0xac–0xb4`, and `s2mpu_fault_handler+0x1dc–0x26c` with the L1 permission string relocations at `.rodata+0xbe8–0xc00` and granule strings at `.rodata+0x32/+0x397/+0x43a`.

A **4 KiB SMPT page entry occupies four bits**. For offset `a = PA & 0x3fffffff`, the 32-bit word index is `a >> 15` and nibble shift is `4*((a >> 12) & 7)`. Within each nibble bit 2 is requester read, bit 3 requester write. Canonical nibble values are `0x0/0x4/0x8/0xc` for deny/R/W/RW. Partial-word updates preserve the low two bits of each nibble, and full-word fills repeat the byte `00/44/88/cc` respectively. A 128 KiB table covers 262,144 pages. Evidence is `smpt_size+0x0`, `pte_from_addr_smpt+0x0–0x14`, `__set_smpt_range+0x40–0x54/+0x168–0x1fc/+0x200–0x26c`, and `.hyp.rodata+0x1b0–0x1cf`. R/W labels follow the common permission index used for FMPT and SMPT.

## Initialization and DMA range update

1. **Allocate and register.** Host allocates order-5 blocks for the supported GiB indices and passes a `0x600`-byte descriptor block to `pkvm_iommu_driver_init` (`pkvm_iommu_s2mpu_init+0x8c–0xb8/+0x11c–0x138`). Hyp copies the block, resolves MPT ops, converts pointers, checks table alignment against `size-1` and invokes a hyp callback with PFN and page count. It records the converted pointer, uniform flag 1 and permission 3 (RW). Initial effective permissions therefore come from the uniform descriptors. The PFN callback implementation is external. Evidence is `s2mpu_init+0x24–0x68/+0x84–0xec`.
2. **Initialize hardware on hyp resume.** Skip on software flag bit 1. Otherwise write the computed or cached context word to `0x104`, program SMPT addresses and FMPT attrs for all eight VID banks using the same per-GiB table pointer across VIDs, then issue all invalidation and sync. SMPT pointer writes follow `dmb oshst`, and the SMPT-mode attr write has another `dmb oshst` before it. The final writes in order are `[0x70]=0xff`, `[0x20]=0xff`, `[0x00]=0`, `[0x50]=0xff`, `[0x10]=0`. Hyp suspend uses uniform permission zero across those banks, invalidates, and makes the same final five writes. Evidence is `s2mpu_resume+0x10–0x88`, `__initialize_ctx+0x0–0x1a0`, `init_with_mpt+0x44–0xd8`, `s2mpu_suspend+0x14–0x80`, and `init_with_prot+0x0–0x140`.
3. **Prepare a host-stage-2 range.** This is the shipped analogue to the source's open operation. Round the start down and compute the rounded end as `(min(end, 0x1000000000) + 0xfff) & 0x3ffffff000`. For a nonempty result pass the inclusive end (rounded end minus 1) and the protection. Permission bit 0 comes from input protection bit 2, and permission bit 1 from input bit 1. Evidence is `s2mpu_host_stage2_idmap_prepare+0x0–0x50`.
4. **Prepare each GiB descriptor.** A full-GiB change records uniform permission. A partial span already covered by the same uniform permission needs no update. For a changed span in a uniform GiB, fill the prefix with the old permission, the span with the new permission, then the suffix with the old permission, and use SMPT mode. For a previously nonuniform GiB, update the span and test the whole table for uniformity at the new permission. For a changed table that stays in SMPT mode the code invokes hyp ops slot `+0x50` with the table pointer and length `0x20000`. Cache clean is the inferred purpose, with the implementation external to this module. Evidence is `prepare_range+0x24–0xf8/+0xfc–0x1d8/+0x1dc–0x220`.
5. **Apply and invalidate.** For a nonempty range with software flag bit 1 clear, write required FMPT attrs across all eight banks. Descriptor update flag bit 0 gates the attr store, so an SMPT-content-only change reuses the programmed pointer and attr. Entering SMPT mode executes `dmb oshst` before the attr store. Write start PFN to `0x1024`, inclusive end PFN to `0x1028`, then 1 to `0x1020`. Issue sync at `0x102c` when software flag bit 0 is set or at each registered child sync base otherwise. The built-in sync flag is derived from the corresponding DT property (`s2mpu_probe+0x10c–0x124/+0x184–0x1a8`, string `.rodata+0x3fc`). The supplied HSI2 node has no such property and no child-sync list. Evidence is `s2mpu_host_stage2_idmap_apply+0x14–0xe4` and `apply_range+0x1c–0xa4`.
6. **Complete.** Run the sync helper for the selected sync blocks, retrying the command if status bit 0 stays clear. The helper returns even on exhaustion of the last retry budget. With no child sync entries, the child path skips those calls. Then run the conditional STATUS poll described above. Evidence is `s2mpu_host_stage2_idmap_complete+0x14–0x90` and `__invalidation_barrier_slow+0x0–0x140`.

## Read-only inspection

Use naturally aligned 32-bit MMIO reads if the block is accessible in the running environment. Below are **driver-observed load sites**. Static evidence establishes intended read use, while actual accessibility and silicon read side effects remain untested. The host read helper uses a 32-bit load at `readl_relaxed+0x44`. The protected host's trap forwarding checks access width and alignment and forwards ordinary reads through `s2mpu_host_dabt_handler+0x4–0x14/+0x68–0x80`.

- Direct read candidates are NUM_CONTEXT, STATUS, implemented FMPT attributes and SMPT pointers, and the direct debug capability registers above. Sync status is read at `0x1030` in the built-in configuration and `+4` in the child configuration. Evidence is `__initialize_ctx+0x8–0x14`, `__all_invalidation+0xd4–0xe8`, `s2mpu_fault_handler+0x208–0x234/+0x338–0x354/+0x390–0x3bc/+0x40c–0x41c/+0x5fc–0x614`, and `__invalidation_barrier_slow+0x0–0x8`.
- For fault state, read `0x2000` first. When a VID bit is pending, read that slot's PA high then low and the three detail words. Leave acknowledgment at `0x2c` untouched during a read-only snapshot. A concurrent handler can acknowledge between reads. Evidence is `s2mpu_fault_handler+0x48–0x108/+0x280–0x2a4`.
- The evidence for the control candidates, context word and invalidation commands consists of writes. Their readback is unverified. The full cache-entry dump requires selector writes at `0x3000`, `0x3014` and `0x3030` (`s2mpu_fault_handler+0x530–0x564/+0x764–0x7bc/+0x874–0x8a4`). The main VERSION/INFO offset remains unresolved.

## Evidence provenance

LLVM objdump, nm and readelf were used directly because REA provider tooling was unavailable. Module SHA-256 values:

- pkvm-s2mpu-v9.ko — b476ea667d8951e6bbe37c8ef7dc9ddef0af1f267c0590a80df6233258ac0841
- pcie-exynos-gs.ko — ef437927ab935f6c57e23a6c6a5996bfb05ea4769275526fed3c22a7fd64b45e

## Observed on the device (probe wifi10, read only, as the bootloader leaves it)

docs/probe-log-wifi10.txt. `0x000` 0, `0x010` 0, `0x020` 0, `0x028` 0xff, `0x050` 0, `0x060` 0x90000000, `0x070` 0,
`0x078` 0xff, `0x100` NUM_CONTEXT 8, `0x104` 0xfedcba98 (all eight contexts valid), `0x1030` sync status 1,
`0x2000` fault status 0. VID 0 L1: attrs at `0x4004/0x400c/0x4014/0x401c` (GiB 0-3) all 0, SMPT pointers hold
stale-looking values. `0x8000` does not answer.

Read: the L1 attrs say "deny" for the DRAM GiB, yet our own UFS DMA (UFS 0x13200000 shares HSI2 with PCIe RC1)
works under iOS with this state, so the block is not enforcing as left by the bootloader (the hyp enables it with
the 0x70/0x20/0x50 = 0xff writes, which are 0 here; this last part is inferred). Consequence: once PCIe bus master
is on, the BCM4390 can reach all of RAM; the firmware only DMAs to addresses the host hands it, so the host must
give it nothing but its own buffers and keep MSI off. Restricting HSI2 later must also keep UFS buffers open.
