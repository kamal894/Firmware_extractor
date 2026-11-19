# BCM4390 rev 5 NCI EROM

The capture identifies **24 complete interface groups plus one partial group**. Core IDs, units, and revisions require the separate CoreInfo tables. The captured words alone cannot bind CA7 or SYSMEM to an exact DMP or wrapper address. Input is [probe-log-wifi6.txt](probe-log-wifi6.txt#L1131).

## Captured topology

`nci_init` masks `0x180070b0` to EROM1 base `0x18007000` and OOBR base `0x18006000`. `nci_initial_parse` uses the first interface word 1, bit 27 and bits 12:0, to select a table and find CoreInfo and InterfaceCfg. CoreInfo supplies ID and revision, InterfaceCfg supplies the interface count, and prior matching IDs determine the unit. The table groups the captured descriptors by their table references. The full scan also inserts the skipped OOBR slot `0x5c0`, whose unit is 0 by construction. [nciutils.c, lines 48–100, 397–426, 489–506, 615–657, 720–770, 774–959 and 1099–1192](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/nciutils.c#895)

All numbers below are hex. CoreInfo and OOBR DMP columns are offsets from their named table base. Wrapper/node pointers and slave ranges are absolute backplane addresses. Ranges are inclusive. Node pointers are in descriptor order and include shared PMNI nodes.

| EROM2 offset | CoreInfo table + offset | OOBR DMP control offset | Wrapper/interface nodes | Slave backplane ranges |
|---|---|---|---|---|
| 000 | OOBR +140 | 148 | 1851a000, 1851e000, 1851c000 | 18000000–18000fff, 1c000000–1fffffff |
| 020 | OOBR +1c0 | 1c8 | 1851b000, 1851e000, 1851d000 | 18001000–18003fff, 08000000–0fffffff, 64-bit descriptor¹ |
| 054 | OOBR +240 | 248 | 18504000, 18516000 | 18080000–18080fff |
| 068 | OOBR +2c0 | 2c8 | 1850b000, 1850c000, 1850d000, 18516000, 18511000 | 18082000–18083fff, e8000000–e87fffff |
| 098 | OOBR +340 | 348 | 18508000, 18509000, 1850a000, 18516000, 18510000 | 18084000–18085fff, e8800000–e8ffffff |
| 0c8 | OOBR +3c0 | 3c8 | 1850e000, 18516000, 18512000 | 18086000–18087fff, e9000000–e97fffff |
| 0e8 | OOBR +440 | 448 | 1850f000, 18516000 | 18081000–18081fff |
| 0fc | OOBR +4c0 | 4c8 | 18516000, 18505000 | 18088000–18088fff, 00000000–07ffffff |
| 114 | OOBR +540 | 548 | 18523000, 18524000 | 19000000–19ffffff |
| — | OOBR +5c0 | 5c8² | — | — |
| 128 | OOBR +640 | 648 | 1851f000, 18528000, 18527000 | 18010000–18014fff, 1a000000–1a01efff |
| 150 | EROM1 +00 | — | 1851f000 | 1808d000–180e2fff |
| 160 | EROM1 +08 | — | 18514000 | e7800000–e79fffff |
| 16c | EROM1 +10 | — | 18513000 | e7a00000–e7bfffff |
| 178 | EROM1 +18 | — | 18515000 | e7c00000–e7ffffff |
| 184 | EROM1 +20 | — | 1851f000 | 18018000–18018fff |
| 190 | EROM1 +28 | — | 1851f000 | 18020000–18027fff |
| 19c | EROM1 +30 | — | 1851f000 | 18019000–18019fff |
| 1a8 | EROM1 +38 | — | 18516000 | 18089000–18089fff |
| 1b4 | EROM1 +40 | — | 1851e000 | 18004000–18004fff |
| 1c0 | EROM1 +48 | — | 1851e000 | 18005000–18005fff |
| 1cc | EROM1 +50 | — | 1851e000 | 18006000–18007fff |
| 1d8 | EROM1 +58 | — | 1851e000 | 18009000–18009fff |
| 1e4 | EROM1 +60 | — | 1851e000 | 1800a000–1800afff |
| 1f0 | EROM1 +68 | — | 1851e000 | 1800b000–1800bfff |
| 1fc | Missing word 1 | — | 1851e000 | Unknown |

¹ At EROM2 +044 the parser stores `addrl=00000000`, `addrh=80000000`, `extaddrl=ffffffff`, `extaddrh=ffffffff`. The two all-ones words are address data.  
² Stored by `nci_check_extra_core`; this entry has no EROM2 interface descriptors to select a wrapper.

The existing probe independently identifies `18000000` as ChipCommon and shows the PCIe registers at `18001000`. Their CoreInfo words are still needed for core revisions and formal parser output. The chip revision 5 is separate from each core revision.

### Additional read coverage

- **EROM1** — Read `0xb0` bytes at `0x18007000`. The parser derives 22 eight-byte records from the EROM pointer.
- **OOBR** — Read CoreInfo and InterfaceCfg (8 bytes per slot) at `0x18006140`, then every `0x80` through `0x18006640` inclusive. That is **88 bytes** for the 11 slots implicated so far. Read OOBR `capability` for the total count and follow any additional references from the EROM2 continuation.
- **EROM2** — Resume at `0x180072b0` (+0x200), where the partial interface's word 1 belongs. The minimum next read is 4 bytes. The exact tail length is unknown. Continue until the parser sees `0xffffffff` at an outer-loop descriptor boundary or a core's final interface has WordOffset zero. **0xd50 bytes** covers the remainder of the current 4 KiB window. [nciutils.c, lines 284–288, 963–1018 and 1120–1192](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/nciutils.c#1120)

## CA7 (0x847) and SYSMEM (0x849)

For each confirmed OOBR core, let **C** be its absolute CoreInfo address and **W** the node pointer of the first Booker interface (BP ID 0 on BCM4390). The default core selection uses that wrapper. The reset routine visits interfaces in reverse order and skips shared PMNI interfaces. [nciutils.c, lines 662–696, 1801–1854, 1942–1953 and 2189–2279](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/nciutils.c#2189)

| Register | Backplane address after identifying the core | Bits |
|---|---|---|
| DMP control | C + 0x08 | Clock enable bit 0 (`0x01`), force gated clock bit 1 (`0x02`), CA7 CPUHALT bit 5 (`0x20`). SYSMEM has the clock controls, with no CPUHALT interpretation established. |
| IDM reset control and driver reset readback | W + 0x140, inferred from the Arm NI-700 layout | Reset entry bit 0. Read 0 for out of reset, 1 for in reset. |
| Separate IDM reset status | W + 0x144, inferred from the same layout | Diagnostic register. Bits 5:4 report reset-exit state in the Arm layout. The driver polls reset control bit 0 for entry and exit and uses that same bit for `nci_iscoreup`. |

`nci_core_cflags` temporarily selects OOBR via PCI config **0x80**, accesses DMP control in BAR0's first 4 KiB, then restores the window. Slice 0 maps **W** through **PCIE2_BAR0_WIN2 at config 0x70**, with wrapper MMIO in BAR0's second 4 KiB (inferred offsets `+0x1140` and `+0x1144`). `nci_core_reset_iface` also temporarily selects OOBR in config 0x80, while its reset access is through `curwrap`. `nci_iscoreup` reads that wrapper and may request the power domain first. Local `siutils.c` dispatches these operations to NCI, and `dhd_pcie.c:11616–11633` checks CA7 unit 0 using the two reads below. [nciutils.c, lines 1835–1854, 2120–2183, 2305–2320 and 2572–2642](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/nciutils.c#2572) · [BCM4390 clock masks](https://android.googlesource.com/kernel/google-modules/wlan/bcmdhd/bcm4390/+/2f66b21f47d4f6ab5bd87511c894b1782d04e742/include/hndsoc.h) · [BCMDHD PCI configuration constants](https://android.googlesource.com/kernel/msm/+/b25668b0a8d7683319d5d8210b417dbb205d1c7f/drivers/net/wireless/bcmdhd/include/pcicfg.h) · [Arm NI-700 IDM register map](https://developer.arm.com/documentation/101566/latest/)

### Minimal read-only CA7 check

After confirming CA7's CoreInfo record and wrapper, and with its power domain accessible, read two LE32 words through the windows above. Window selection involves PCI config writes.

1. Read IDM reset control. `value & 0x01 == 0` means up under DHD's test. `0x01` means in reset.
2. Read DMP control. `value & 0x20 == 0x20` means halt asserted under DHD's test. Zero means halt clear. A clock-enabled, force-gate-clear, halted core gives `value & 0x23 == 0x21`. Treat an all-ones read as inconclusive.

Direct curl failed DNS resolution. The pinned Gitiles source was readable through the browser fetch. The pinned IDM layout and PCI configuration header could not be retrieved, so the IDM offsets use Arm's NI-700 layout and the PCI config offset is corroborated by Broadcom's BCMDHD header from another revision.
