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
