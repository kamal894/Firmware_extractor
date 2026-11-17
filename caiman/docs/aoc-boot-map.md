# How Linux starts the AOC (caiman, gsa-enabled)

This map was read statically from the Linux modules in the vendor_boot ramdisk
(`aoc_core.ko`, `gsa.ko`, `aoc_tbn_service_dev.ko`, `touch_bus_negotiator.ko`) and from the live
DT. Offsets are `.text` offsets in each module. Anything not checked on hardware says so.

## 1. GSA mailbox (gsa.ko; DT `gsa@16490000`, irq SPI 0x177)

`gsa_send_mbox_cmd_locked` (0x20e0) does the following, in order:

1. Writes the command to **+0x80** (0x2188) and the argument count to **+0x84** (0x2198, at most
   16). Then it writes the arguments to **+0x88 + 4*i** (loop at 0x21a8).
2. Writes **+0x24 = 1** (0x21e0) to clear the previous interrupt.
3. Clears bit 0 of the interrupt mask at **+0x28** (0x2204-0x2210) to unmask the reply interrupt.
4. Writes **+0x40 = 1** (0x2230), the doorbell to GSA.
5. Waits for the reply. In Linux, `gsa_mb_irq_handler` (0x1db8) tests **+0x30 bit 0** (0x1e20),
   sets mask bit 0 again and completes. Without interrupts, polling +0x30 bit 0 should do the same
   (not tried on hardware).
6. Reads the reply:
   - **+0x80** must be cmd | 0x80000000 (0x230c);
   - **+0x84** is the status, 0 for OK (0x231c); 0x82 means -ETIMEDOUT (0x237c);
   - **+0x88** is the reply count; the reply words follow at **+0x8c + 4*i** (0x228c).
7. Writes +0x24 = 1 and sets +0x28 bit 0 (0x22b4-0x22ec).

`gsa_mbox_init` sets +0x28 = 0xffff (0x1c88), which masks everything.

Power wrapper: `gsa_send_mbox_cmd` (0x1e84) brackets commands 1..0xb4 when its reference count is
0. It first sends **0x96** (wake, no arguments, 0x1f08) and afterwards **0x97** (sleep, 0x1f8c).

### Commands that go through the mailbox

| Function | Cmd | Arguments |
|---|---|---|
| `gsa_load_aoc_fw_image` (0xcc) | **0x5a** | 4 words: header DMA address low and high, then firmware phys low and high (0xe0-0x108) |

## 2. GSA's Trusty service (not the mailbox)

`gsa_send_aoc_cmd` (0x1e8) and `gsa_unload_aoc_fw_image` (0x144) go through
`gsa_tz_chan_msg_xchg` (0x2488). That function uses **trusty-ipc** (tipc_create_channel,
tipc_chan_connect, tipc_chan_queue_msg) to reach the Trusty port
**`com.android.trusty.gsa.hwmgr.aoc`**.

| Function | Request | Reply |
|---|---|---|
| `gsa_send_aoc_cmd(state)` | 8 bytes: `{ 1, state }` (0x1fc-0x21c) | 12 bytes: `{ 0x80000001, 0 = OK, result }` (0x238-0x258) |
| `gsa_unload_aoc_fw_image` | 4 bytes: `{ 2 }` | 8 bytes: `{ 0x80000002, 0 }` |

Starting the AOC therefore needs a Trusty IPC client: trusty SMCs, a virtio device with vrings and
the TIPC control protocol. The open-source Linux `drivers/trusty` (trusty.c, trusty-virtio.c,
trusty-ipc.c) is the reference.

## 3. aoc_fw_callback (aoc_core.ko 0x2a4), in order

1. Reads DT properties and the chip id (0x314-0x60c).
2. `request_aoc_on(aoc, 1)` (0x6dc). In our boot aoc_req already reads 1.
3. Checks the image: `_aoc_fw_is_valid`, `_aoc_fw_ipc_offset`, `_aoc_fw_version`, `is_release`,
   `is_compatible`, `is_signed` (0x6f8-0x82c).
4. `aoc_control` = carveout virtual address + ipc offset (0x844-0x878), then `_aoc_fw_commit`
   (0x87c). This copies the image into the DRAM carveout (DT `memory-region`).
5. `aoc_configure_iommu` (0x8bc): maps the AOC SysMMU entries from the firmware's own iommu table
   (`_aoc_fw_iommu_offset`, `_size`, `_entry`). Each entry is
   `iommu_map(iova = e << 12, pa = (e >> 8) & 0xffffff000, size = (e >> 32) & 0xfffff000, prot 3)`
   (0x18ac-0x18cc).
6. On the GSA path (0x92c onward):
   1. `aoc_fw_authenticate` (0x93c, function at 0x16f0) copies the **first 0x1000 bytes of aoc.bin**
      into a DMA buffer. It then calls GSA mailbox cmd 0x5a with (that buffer, carveout phys at
      `aoc+0x50`).
   2. `aoc_pass_fw_information` (0x960) writes the boot information into the IPC area
      (`aoc_control`).
   3. `disable_power_mode` (0x984).
   4. `aoc_release_from_reset` (0x9ac): sets bit 0 of the SRAM word at `aoc_sram_translate(aoc+0x47c)`.
   5. **`gsa_send_aoc_cmd(1)`** (0x9bc) through Trusty: the start.
   6. `configure_crash_interrupts`, then a delayed work that checks the AOC came online after
      1250 jiffies (0x9dc-0xa00), `msleep(2000)`, and `enable_power_mode` (0xa24).

`write_reset_trampoline` (0x8c4) is on the non-GSA path only.

## 4. Giving the touch bus to the AP

- `touch_bus_negotiator.ko` talks to the AOC service **`com.google.tbn_service`**
  (`aoc_tbn_service_dev.ko`: `aoc_tbn_service_write` / `_read`).
- The message format is not read out yet.
- It needs the AOC IPC rings (`aoc_service_write`) to be working, so the AOC has to be fully online.

## 5. What to avoid

gsa.ko also exports the following. None of them is needed; do not send them:
- `gsa_kdn_*`: key derivation and programming;
- `gsa_sjtag_*`: secure JTAG;
- the TPU and DSP image commands.

The AOC needs only:
- mailbox 0x96, 0x5a and 0x97;
- Trusty `hwmgr.aoc` messages 1 (state) and 2 (unload).

## 6. Minimum work to start the AOC from our boot

1. Copy aoc.bin into the carveout, as `_aoc_fw_commit` does (to read: aoc_core.ko 0x76c4).
2. Program the AOC SysMMU from the firmware's iommu table.
3. GSA mailbox: 0x96, then 0x5a (header, carveout), then 0x97.
4. `aoc_pass_fw_information` (to read: 0x1a44), then the SRAM reset bit.
5. **A Trusty IPC client** to send `{1, 1}` to `com.android.trusty.gsa.hwmgr.aoc`. This is the
   largest part.
6. AOC IPC (rings and mailboxes 0x15200000...), then `com.google.tbn_service` to ask for the bus.

## 7. Gates checked on hardware (PROBE_TOUCH=5, 2026-11-17)

- **Trusty is alive in our boot.** `SMC_FC_GET_VERSION_STR` (0xbc00000a) returns length 0x44:
  "Project: ripcurrentpro, Build: 15189916, Built: 12:53:31 Apr 12 2026". The bootloader log
  shows it selecting Trusty API version 3.
- **The GSA mailbox answers when polled.** It had last been left at 0x80000041 by the bootloader.
  - 0x96: reply 0x80000096, status 0, after 3 ms;
  - 0x97: reply 0x80000097, status 0, after 1 ms.

## 8. A Trusty client from our boot: QL-TIPC (probe/trusty.c, 2026-11-17)

Bootloaders talk to Trusty through QL-TIPC (Trusty lib/trusty/tipc_dev_ql.c), not virtio:
- one shared page;
- std calls `SMC_SC_CREATE_QL_TIPC_DEV` (0x3200001e), `SHUTDOWN` (0x3200001f) and
  `HANDLE_QL_TIPC_DEV_CMD` (0x32000020);
- commands CONNECT 1, GET_EVENT 2, SEND 3, RECV 4, DISCONNECT 5;
- replies carry opcode | 0x8000.

Steps that work on hardware:
1. **`SMC_FC_API_VERSION(5)` = 5.** Asking for 3 also returns 5: something after the bootloader
   has already locked 5. So the page is shared with FF-A 1.0:
   - FFA_VERSION 0x10000;
   - FEATURES(MEM_SHARE) succeeds;
   - ID_GET returns 0;
   - RXTX_MAP64 of two pages;
   - MEM_SHARE of one page to endpoint 0x8000, which gives handle 0xffffffc0.
2. **The page must be shared cached** (attributes 0x2f). Sharing it uncached (0x27) made CREATE fail
   with -7. Our MMU is off, so the page is cleaned and invalidated by VA (`dc civac`) around every
   call.
3. **CONNECT must send the name with its NUL.** Trusty requires strnlen(name) < length. Without the
   NUL every port gave -2.
4. **Results:**
   - `hwmgr.aoc`, `hwmgr.tpu`, `hwmgr.dsp`, `hdcp.auth` and `apploader` connect (handle 0x20);
   - `hwkey` and a made-up name give -7;
   - `hwmgr.aoc` then reports **READY** (event 0x1). DISCONNECT, SHUTDOWN, MEM_RECLAIM and
     RXTX_UNMAP all succeed.

The client is now in place for `gsa_send_aoc_cmd`: SEND `{1, state}` and RECV
`{0x80000001, 0, result}`. Nothing has been sent to the AOC service yet.

## 9. aoc.bin layout and what Linux writes (read statically, 2026-11-17)

- **Header checks:**
  - +0x400 must be "AOC\0" (0x434f41) and +0x404 the header version (2 here).
  - The firmware header at +0x1000 starts with 0xaabbccdd (`_aoc_fw_is_valid` 0x6ebc).
- **Header words at +0x1000 in this aoc.bin:**

  | Offset | Value | Meaning |
  |---|---|---|
  | +0x4 | 1 | release flag (`_aoc_fw_is_release`) |
  | +0xc | 0x1c00000 | **ipc offset** (`_aoc_fw_ipc_offset`) |
  | +0x14 | 0x98000000 | — |
  | +0x18 | 0x1000 | bootloader offset (`_aoc_fw_bootloader_offset`) |
  | +0x1c..+0x24 | 0x7000, 0x8000, 0x2290 | — |
  | +0x30 | "15231849-polygon" | version string |

- **Image config at +0x580** (v2; +0x560 for v1):
  - +0x10: bootloader blob offset 0x40, size 0x80;
  - +0x14: iommu table offset 0xc0, size 0x38, so 7 entries.
  - Each entry decodes as `iova = e << 12` (low 32 bits), `pa = (e >> 8) & 0xffffff000`,
    `size = (e >> 32) & 0xfffff000`:

  | iova | pa | size | Note |
  |---|---|---|---|
  | 0x98000000 | 0x94000000 | 0x3000000 | the carveout, DT `aoc_reserve` |
  | 0x9b000000 | 0x97000000 | 0x400000 | |
  | 0x9e000000 | 0x15100000 | 0x200000 | |
  | 0x9e200000 | 0x16400000 | 0x100000 | |
  | 0x9e500000 | 0x11200000 | 0x100000 | |
  | 0x9e300000 | 0x40000000 | 0x80000 | |
  | 0x9e380000 | 0x40400000 | 0x80000 | |

  The AOC SysMMU is `sysmmu_aoc@19090000`, compatible "samsung,sysmmu-v9" (samsung_iommu_v9.ko).
  Our boot would have to program it.
- **`_aoc_fw_commit`** (0x76c4): `memcpy(carveout, aoc.bin + 0x1000, size - 0x1000)`.
- **`aoc_pass_fw_information`** (0x1a44) writes at carveout + ipc offset (pa 0x95c00000):
  - the words 0x0a0cda7a, 22, 0x114;
  - then 22 entries of `{key, 4, value}`, with keys 0x1001..0x1017. The values come from
    `aoc_fw_callback` 0x2d8-0x530: carveout base/size, board id/rev, chip id/revision/type,
    `get_random_u32`, DT properties. The mapping of each key to its value is not finished.
- **Do not use `mem dump` on carveout addresses through the AOC console.** 0x99c00000 is not mapped
  in the A32 core's MMU. Reading it crashed the AOC (CONSOLE task, DFAR 0x99c00000, DFSR 5).
  Android restarted it.

## 10. Correction: on the GSA path Linux does less (checked 2026-11-17)

Codex's analysis is in docs/aoc-fw-load.md. Checked against the disassembly:
- `aoc_fw_callback` 0x8a8 calls `of_find_property(.., "gsa-enabled")` (.rodata+0x5e4).
- At 0x8b0, `cbnz` goes to 0x92c. `aoc_configure_iommu` and `write_reset_trampoline` (0x8bc, 0x8c4)
  run only **without** GSA.
- At 0x9a4, `cbnz` goes to 0x9b4. `aoc_release_from_reset` (0x9ac) also runs only without GSA.
  With GSA the start is `gsa_send_aoc_cmd(1)` alone.

So on caiman, which has `gsa-enabled`, Linux does this:
1. `request_aoc_on`: write 1 to aoc_req at 0x154b0000.
2. Check the image, then copy aoc.bin+0x1000 to the carveout at 0x94000000 (0x1bb9ac0 bytes).
3. GSA mailbox 0x5a(DMA address of a copy of aoc.bin[0:0x1000], 0x94000000).
4. Write the fw-info block at 0x95c00000. The table of 22 entries is in docs/aoc-fw-load.md §A.
   Keys 0x1011-0x1013 and 0x1016 (random, chip revision, type, product) are still unknown.
5. `disable_power_mode(0, 1)`. This is an import from another module, not yet found.
6. Trusty `hwmgr.aoc` `{1, 1}`. Then Linux sleeps 2 s and calls `enable_power_mode(0, 1)`.

The AOC SysMMU, the reset trampoline and the SRAM reset bit are presumably set up by GSA. We do not
program them. The SysMMU details in aoc-fw-load.md §C are kept only as a fallback.
