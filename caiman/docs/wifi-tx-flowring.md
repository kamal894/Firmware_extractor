# BCM4390 Ethernet TX over PCIe msgbuf

For firmware 27.10.1121.57.18, IPC rev 9, interface 0, use control submit ring 0 to open a TX flow, post Ethernet TX items to that flow, and reap packet IDs from TX complete ring 3. Multibyte descriptor and index fields are little-endian. DMA addresses are the device-visible address, low 32 bits followed by high 32 bits. Copy Ethernet bytes verbatim. Citations refer to the DHD files in `~/caiman-work/ref/dhd`, the probe at `probe/wifi.c`, and an existing probe log. Firmware-specific inferences are labeled below.

## 1. Ring IDs and DMA indices

For flow ordinal `k`, use **flow ID `f = 2 + k`** in create requests and completion matching, **msgbuf ring ID `r = 5 + k`** in DB0, and **H2D DMA slot `f`**. First flow: ID 2, DB0 ring 5, slot 2. The two common H2D slots come first, but the msgbuf ring ID space also includes the three common D2H rings. DHD explicitly converts the flow ID to the global ring ID when attaching the ring and converts back for H2D DMA offsets. (`bcmpcie.h:184–196,232–247`, `dhd_msgbuf.c:309–335,429–433,13425–13432`.)

| Offset in ring_info | Size | Field |
|---|---:|---|
| 0x00 | 4 | TCM ringmem pointer |
| 0x04 / 0x08 / 0x0c / 0x10 | 4 each | TCM H2D WR / H2D RD / D2H WR / D2H RD array pointers |
| 0x14 / 0x1c / 0x24 / 0x2c | 8 each | Host DMA array addresses in the same order |
| 0x34 | 2 | max_tx_flowrings |
| 0x36 | 2 | max_submission_queues (DHD max_submission_rings) |
| 0x38 | 2 | max_completion_rings |
| 0x3a | 2 | max_vdevs |

DHD reads the struct through the shared TCM pointer (shared +0x30 in the probe), then decodes the three counts separately on rev ≥6. The saved dump shows `0x002b0028` at +0x34 and `0x00000004` at +0x38: 40 flows (IDs 2–41), 43 submission slots, four completion slots. The DHD pointer walk accounts for an extra H2D debug ring after the flows. Size both H2D arrays for the advertised submission count, both D2H arrays for the completion count. Here 43 × 4 = 172 bytes fits the probe's 256-byte spacing. (`bcmpcie.h:311–345`, `dhd_pcie.c:15277–15304,15331–15353,15632–15650`, `probe/wifi.c:131–132,473–505,903`, `docs/probe-log-wifi10-ioctl.txt:108–111`.)

| Index | Host block offset | TCM mirror address if using the probe's dual publication |
|---|---|---|
| Flow WR / RD | 0x1a000 + 4*f / 0x1a100 + 4*f | h2d_w_idx_ptr / h2d_r_idx_ptr + 4*f respectively |
| Control completion WR / RD | 0x1a200 / 0x1a300 | d2h_w_idx_ptr / d2h_r_idx_ptr |
| TX completion WR / RD | 0x1a204 / 0x1a304 | d2h_w_idx_ptr / d2h_r_idx_ptr + 4 respectively |

First flow WR/RD are +0x1a008/+0x1a108. Start with zeroed host arrays and ring memory. Publish flow WR=0 before the create request. After initialization, host owns H2D WR and D2H RD, firmware owns H2D RD and D2H WR. DHD uses the host DMA array for each direction in this mode. Mirroring host-owned indices into TCM follows the working probe convention. (`dhd_msgbuf.c:1437–1450,14746–14759,14846–14859,13779–13791,13882–13908`, `probe/wifi.c:168–190,252–261,492–505`.)

## 2. Open a flow on control submit ring 0

Wait for the STA association and WPA2 key installation before sending DHCP. Interface 0 already exists in DHD; `WLC_E_IF` updates host interface/role bookkeeping and `WLC_E_LINK` updates link state. Use interface 0's configured MAC for SA (probe `cur_etheraddr`). For a first broadcast DHCP frame use Ethernet DA `ff:ff:ff:ff:ff:ff`, TID 0, packet priority 0. **Inference:** a dedicated broadcast flow with that exact DA is the minimal test candidate. For subsequent distinct destinations, conservatively create a separate flow per (if_id, TID, DA). A universal broadcast/default flow or STA DA collapsing to a single flow is unverified here. DHD consumes the flow ID already tagged on the packet, and DHD's station cache can map multiple packet priorities to one flow AC priority. `dhd_flowring.c` is missing from the supplied reference tree, so the exact `dhd_flowid_get` role-specific lookup and multicast/default policy cannot be established from that tree. (`dhd_common.c:6219–6254,6389–6396`, `dhd_linux.c:2179–2240`, `dhd_pcie.c:6539–6544,6604–6612,16518–16522`, `probe/wifi.c:568–581`.)

Allocate a zeroed, DMA-visible work-item array, at least 8-byte aligned, with `N × stride` bytes. Example `N=512` (24 KiB at stride 48); DHD has 512 and 768 item profiles, so 512 is a host choice, not an IPC rev 9 constant. Use DMA memory disjoint from existing rings, indices and packet buffers. Send this 40-byte `tx_flowring_create_request_t`:

| Offset | Bytes | Field and value |
|---|---:|---|
| 0x00 | 1 | msg_type = 0x03 |
| 0x01 | 1 | if_id = 0 |
| 0x02 | 1 | control ring's phase flag. DHD starts the first lap at 0x80 and toggles bit 7 on return to slot 0. The probe retains its current control flags convention. |
| 0x03 | 1 | current control submit epoch, advance modulo 253 |
| 0x04 | 4 | request_id = 0 |
| 0x08 | 6 | DA, broadcast for this example |
| 0x0e | 6 | SA, interface MAC |
| 0x14 | 1 | tid = 0 for this flow |
| 0x15 | 1 | if_flags = 0 (ordinary STA traffic) |
| 0x16 | 2 | flow_ring_id = 2 for the first flow |
| 0x18 | 1 | tc = 0 (zero default, no assignment in ordinary create path) |
| 0x19 | 1 | priority_ifrmmask = 0 without IFRM. DHD sets a core mask only for IFRM, not a priority. |
| 0x1a | 2 | int_vector = 0 (zero default, no assignment in ordinary create path) |
| 0x1c | 2 | max_items = N (512 → 00 02) |
| 0x1e | 2 | len_item = stride (48 → 30 00) |
| 0x20 / 0x24 | 4 each | flow_ring_ptr low / high |

Advance and publish control WR through the existing submit path and write DB0 `0xff000000 | ctrl_wr`. Use the current control epoch rather than restarting it when opening a flow. (`bcmmsgbuf.h:60–61,211–222,253–254,551–573`, `dhd_msgbuf.c:567–609,2126–2131,12876–12879,14752–14859`, `probe/wifi.c:168–190`.)

Handle `MSG_TYPE_FLOW_RING_CREATE_CMPLT` (0x04) on control completion ring 2. Layout: common header at +0x00–0x07 (type, if_id, flags, epoch, request_id); signed LE16 status at +0x08; LE16 flow ID at +0x0a; reserved words at +0x0c and +0x10; XOR marker at +0x14. Open the flow only on matching flow ID and status 0. Validate the current control completion epoch and XOR over all six words before consuming. Publish control completion RD through the existing path; the generic DHD DMA-index notification for this ring is `0xdd020000 | rd`. (`bcmmsgbuf.h:705–723,880–888`, `dhd_msgbuf.c:1773–1836,13899–13908,14871–14886`, `dhd_pcie.c:16478–16516`, `probe/wifi.c:375–396`.)

## 3. Post one Ethernet frame to the open flow

Use a 48-byte base item for plain, single-segment TX, with host-computed protocol checksums. Device flags2 `0x4de43001` includes `PCIE_SHARED2_TXPOST_EXT = 0x00400000`. Host cap `0x00821409` lacks `HOSTCAP_TXPOST_EXT = 0x08000000`. DHD mirrors the device bit into its host cap, but its flow stride calculation still returns 48 with no active extension. Active TX CSO adds four bytes then rounds to 56; the header's fixed 56/64-byte extended compile-time forms do not determine this ordinary DHD path. **Inference:** try 48 with the current cap for plain TX. The references do not establish whether this exact firmware requires the host cap acknowledgment even at stride 48. The DHD-equivalent acknowledgment would be `0x08821409` at initial capability publication, with CSO disabled and stride still 48. (`bcmpcie.h:156,470`, `bcmmsgbuf.h:64–85,1148–1244,1829–1831`, `dhd_pcie.c:15511–15519`, `dhd_msgbuf.c:1568–1587,4326–4335`, `probe/wifi.c:486–489`.)

| Offset | Bytes | Field and value |
|---|---:|---|
| 0x00 | 1 | msg_type = 0x0f |
| 0x01 | 1 | if_id = 0 |
| 0x02 | 1 | flow phase, 0x80 on first lap, XOR 0x80 on each subsequent slot-0 allocation |
| 0x03 | 1 | epoch = 0 in a zeroed item. The ordinary DHD TX post path leaves this byte unwritten. Zero is an initialization choice; firmware validation of this byte is unverified. |
| 0x04 | 4 | request_id = unique nonzero packet ID while outstanding |
| 0x08 | 14 | txhdr: DA[6], SA[6], EtherType/length[2] |
| 0x16 | 1 | flags = 0x01 OR ((packet_priority & 7) << 5), so 0x01 for priority 0 |
| 0x17 | 1 | seg_cnt = 1 |
| 0x18 / 0x1c | 4 each | metadata_buf_addr low / high = 0 |
| 0x20 / 0x24 | 4 each | data_buf_addr low / high, DMA address of payload starting at original frame byte 14 |
| 0x28 | 2 | metadata_buf_len = 0 |
| 0x2a | 2 | data_len = frame length minus 14 |
| 0x2c / 0x2d / 0x2e / 0x2f | 1 each | ext_flags / scale_factor / rate / exp_time = 0 for plain TX |

The header's v1 union also names +0x2c as a marker. The ordinary DHD TX path writes ext_flags and has no TX post XOR-generation step. With a negotiated v2 CSO item, +0x30–0x33 is `pktinfo` (version, checksum-type bits, network header length, transport header length) and +0x34–0x37 is padding. The v3 `TX_PCIE_APP` form uses that same pktinfo followed by aggregation flags +0x34, packet count +0x35, total length +0x36 (2), buffer offset +0x38 (2), and padding +0x3a (6). Neither extension is needed by the 48-byte recipe. (`bcmmsgbuf.h:269,1097–1178,1197–1247,1269–1270`, `dhd_msgbuf.c:10425–10436,10474–10483,10547–10578,10669–10694,10731–10734,12876–12879,13546–13557`.)

Reserve slot `wr` only if `next_wr = (wr + 1) % N` differs from the firmware H2D RD. Fill the descriptor at `ring_base + wr*stride`. Make both payload and descriptor visible to DMA before publishing LE32 `next_wr` in flow WR. Order that publication before ringing PCIe core +0x140 with `0xff000000 | (r << 16) | next_wr`. **First post: 0xff050001**. Keep the payload mapped and intact until its packet ID completes. (`dhd_msgbuf.c:124,12837–12879,13752–13813,9780,9837`, `probe/wifi.c:118,183–190,214–223`.)

## 4. Reap TX completions on common ring 3

Poll D2H WR slot 1. Ring 3 has 1024 × 24-byte items at host block +0x7000. For each pending item, synchronize for CPU reads and require the ring's expected epoch at +0x03 and XOR of all six LE32 words equal to zero. DHD initializes each D2H completion sequence to 254, so the first expected epoch is 1, then it advances modulo 253 independently per ring. (`probe/wifi.c:123–126`, `bcmmsgbuf.h:57–58,90`, `dhd_msgbuf.c:1773–1836,2089–2096`.)

| Offset | Bytes | TX_STATUS (0x10) field |
|---|---:|---|
| 0x00 / 0x01 / 0x02 / 0x03 | 1 each | type / if_id / flags / epoch |
| 0x04 | 4 | request_id, echoed packet ID |
| 0x08 | 2 | completion status (or d_t4 in the latency-timestamp variant) |
| 0x0a | 2 | original flow ID (2 for first flow) |
| 0x0c–0x13 | 8 | XOR union view: tref LE32 at +0x0c, d_t2 LE16 at +0x10, d_t3 LE16 at +0x12 |
| 0x14 | 4 | XOR marker |

The alternate TX-status/timesync union view puts metadata length or extended TX status at +0x0c, WLAN TX status at +0x0e, and an 8-byte timestamp at +0x10. Those fields overlay the XOR view. The exact meaning of the non-marker tail on this firmware with plain TX and no timestamps is unverified. Use the echoed packet ID and flow ID to retire the DMA buffer after valid completion. DHD also uses the +0x08 completion status bits for packet-fate handling. (`bcmmsgbuf.h:270,705–723,766–770,1306–1345`, `dhd_msgbuf.c:9742–9748,9780–9799,9837,9900–9915`.)

Advance RD modulo 1024. Publish LE32 RD at host block +0x1a304, mirror to `d2h_r_idx_ptr + 4` when following the probe, then order the stores before DB0 **`0xdd030000 | rd`**. First completion acknowledgment is `0xdd030001`. DHD applies this RD notification generically in DMA-index mode; the probe already uses it for RX ring 4. (`dhd_msgbuf.c:122,13899–13908`, `probe/wifi.c:252–261`.)
