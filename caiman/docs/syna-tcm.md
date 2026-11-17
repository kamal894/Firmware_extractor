# Caiman Synaptics TouchComm: static bring-up findings

Analysis date: 2026-11-17. No device access or execution of target code. Inputs were read only; this document is the only file written.

## Evidence and limits

Primary binary: `/Users/harkamalpreetsingh/caiman-work/vkb/rd/lib/modules/syna_touch.ko`, SHA-256 `6951375adeb43b4a8adbf53a6085f5b0eea919037fb803ccf20615673dadbf37`. Disassembled with `/opt/homebrew/opt/llvm/bin/llvm-objdump -dr`; symbols with `llvm-nm`. All addresses below are **section-relative .text addresses**, not runtime addresses or file offsets. Function-relative offsets are given as `function+offset`.

Local sources: `~/caiman-work/caiman-live.dts` (DT); this repository's `probe/touch.c` and the end sections of `docs/touch-plan.md`. Prior hardware observations quoted here come from that plan/user context, not from this analysis.

The supplied upstream repository name `touch/synaptics` could not be retrieved. The publicly indexed repository is `touch/synaptics_touch`. Shell HTTPS fetching failed DNS resolution. The following older reference files were readable through web retrieval; they are **reference implementations, not proven source of this module**:

- [syna_tcm2_platform_spi.c, android-gs-bluejay-5.10-android13-qpr2](https://android.googlesource.com/kernel/google-modules/touch/synaptics_touch/+/refs/heads/android-gs-bluejay-5.10-android13-qpr2/syna_tcm2_platform_spi.c), blob `202efecef0191576e6503327a8bf567f59d5fdac`.
- [tcm/synaptics_touchcom_core_v2.c, same branch](https://android.googlesource.com/kernel/google-modules/touch/synaptics_touch/+/refs/heads/android-gs-bluejay-5.10-android13-qpr2/tcm/synaptics_touchcom_core_v2.c), blob `edfeecbb64c6bd5d8726cc0f3a03174bbaf6fa57`.

References to `tcm/synaptics_touchcom_core_v1.c`, `synaptics_touchcom_func_base.c`, and touch-report source below identify the source module/function corresponding to the binary symbol; their complete source was not successfully retrieved. Binary evidence is authoritative for those claims. Unknowns and proposed experiments are explicitly identified.

## Findings that change the next probe

1. **Do not equate “not A5” with “not the chip.”** This module supports and auto-detects both TouchComm v1 and v2. V2 has no A5 marker. Nevertheless, `d2 ff ff ff` and `f9 ff ff ff` are invalid v2 headers as well as invalid v1 headers.
2. **The probe really transmits FF on reads.** The build's normal zero-byte-delay SPI path instead supplies a null TX buffer. Explicit FF is only in its byte-at-a-time path. Zero filler is a reasonable next bare-metal matching choice, but the actual Samsung controller's null-TX MOSI behavior has not been proven from `syna_touch.ko` alone.
3. **The binary enables AVDD (LDO4M) before VDD (LDO25M)**; the probe does the reverse. Reset is active low, 2 ms asserted, followed by 50 ms after release.
4. **Four-byte header reads and protocol-aware continuation are needed.** A blind 48-byte read neither implements v1 continuation nor v2's request/response protocol. The module accepts up to 48 bytes of identify *payload*, so 48 total bytes is not a safe fixed identify-message length.
5. **The module preserves the firmware's touch-report configuration.** There is no verified constant default coordinate packet to hard-code.

None of these facts proves the cause of the absent startup IRQ. Bus ownership, physical power, reset reaching the chip, and SPI sampling remain possible causes. Guess: predominantly FF with a changing first byte is an electrical/transaction artifact; static analysis cannot establish that diagnosis.

## 1. SPI parameters

DT node `/spi@111D0000/touchscreen@0` (lines 7839–7930):

| Item | Value/evidence |
|---|---|
| Bus / CS | spi20, device reg 0; SPI controller at 0x111d0000 |
| Mode | `synaptics,spi-mode = 0`: CPOL=0, CPHA=0 |
| Maximum frequency | `spi-max-frequency = 0x989680` = 10,000,000 Hz; this is a ceiling, not proof of the selected physical clock |
| Byte/block delay | `synaptics,spi-byte-delay-us = 0`, `synaptics,spi-block-delay-us = 0` |
| Controller data | feedback-delay=0, chip-select-mode=0, cs-clock-delay=2 |
| Controller | FIFO 64 bytes, dma-mode, swap-mode=1; empty pinctrl-0 |

`syna_tcm2_platform_spi.c:syna_spi_probe`, binary `syna_spi_probe+0x10c..0x144` (0x16740–0x16778), copies a mode <=3 to the SPI device, writes 8 bits per word (packed store of 0x0108 at device+0x3a5), then calls `spi_setup`. No LSB-first selection is evidenced here. Normal byte ordering is MSB-first by SPI convention; the exact controller implementation is outside this module.

`syna_spi_read` at 0x17c20:

- `+0xe4..+0xf8` (0x17d04–0x17d18) tests the configured byte delay.
- Nonzero delay: `+0x100..+0x10c` writes FF into a dummy byte; one transfer per received byte has both TX and RX pointers (`+0x148..+0x15c`). This clocks full-duplex electrically.
- **Zero delay, this DT:** `+0x180..+0x194` (0x17da0–0x17db4) sets transfer TX pointer to NULL, RX pointer to buffer, and length to requested size. It calls `spi_sync` at `+0x1e8`. This is receive-only in the Linux transfer API, with a separate write operation for commands. Do not assume it is a bidirectional single-wire/3-wire bus.
- A build flag at hw_if+0x366 permits reads >63 bytes to round length to a multiple of four and select 32-bit transfer words; <=63-byte reads retain byte transfers (`+0x80..+0xcc`, `+0x198..+0x1b0`). Small initial reads avoid this optimization.
- The inspected transfer structures are zero-initialized, including cs_change and delay fields. The zero-delay path supplies one transfer per `spi_sync`; the touch module does not manually toggle a CS GPIO. Linux/Samsung SPI owns CS. Expected ordinary behavior is CS asserted across one transfer/message, deasserted between separate read/write messages; exact timing and meaning of cs-clock-delay=2 require the matching Samsung SPI driver, not this module. Do not interpret 2 as microseconds without that source.

No nonzero byte/block delay is requested in this DT. Protocol sleeps are separate: v1 continued reads wait 50–100 us (`syna_tcm_v1_continued_read+0x1ac..+0x1b8`, 0x58a8–0x58b4); v2 waits 50–100 us between command write and response read (`syna_tcm_v2_execute_cmd_request+0x114..+0x120`, 0x7034–0x7040). V1 multi-chunk writes contain 500–1000 us waits (0x528c–0x5298 and 0x52fc–0x5308). These are not per-bit timing requirements.

Probe comparison (`probe/touch.c:spi_init`, `spi_read`, lines 290–354): mode 0, byte transfers, manually drives controller CS active low, waits 10 us after assertion, transmits **FF for every byte**, enables TX and RX together, and deasserts CS on return. It does not explicitly select a 10 MHz clock in `spi_init`; functional clock/divider correctness is not established by the mode setup. It preloads all N bytes into a 64-byte FIFO, so a future larger protocol read must stream/refill rather than blindly increase N beyond FIFO capacity.

## 2. Power and reset

DT phandles establish the names unambiguously:

- vdd-supply 0x443 = S2MPG14 LDO25M, `L25M_TSP_DVDD`, 1.8 V.
- avdd-supply 0x444 = S2MPG14 LDO4M, `L4M_TS_AVDD`, 3.3 V.
- IRQ = gpn0 pin 0, property flags 0x2008; `irq-on-state=0` is the explicit raw active level. Do not reinterpret the entire 0x2008 as a simple GPIO active-low flag.
- Reset = gpp1 pin 1, GPIO flags 0; **`reset-on-state=0` makes assertion raw LOW despite GPIO flags=0**.
- reset-active-ms=2, reset-delay-ms=50, legacy power-delay-ms=200.

### Build behavior, including differences from the old reference

`syna_spi_probe` maps avdd-name to global .data+0x2e0 and vdd-name to .data+0x2d0 (`+0xbbc..+0xc14`, 0x171f0–0x17248). Regulator acquisition stores vdd handle at .data+0x2d8 and avdd at .data+0x2e8 (`+0x338`, 0x1696c; `+0x364`, 0x16998). The hw_if object starts at .data+0x1d8.

`syna_spi_power_on+0x2c..+0xb8` loads vdd at hw_if+0x100 into x23 and avdd at +0x110 into x21, **enables x21/AVDD first** at 0x1825c, waits the avdd-on delay if nonzero, then enables x23/VDD at 0x18214. The associated error strings explicitly name avdd and vdd, confirming the mapping. This differs from the older linked reference's `syna_spi_enable_regulator` order. Cite the binary for this build.

Timing nuance: the parser stores legacy power-delay-ms=200 at .data+0x2a8 (hw_if+0xd0), but the power function reads separate avdd-on delay at +0xd4 and vdd-on delay at +0xdc. `syna_spi_probe+0x1110..+0x11a8` initializes these separate delays to zero and optionally reads `synaptics,avdd-power-on-delay-ms` / `synaptics,vdd-power-on-delay-ms`. Neither property is in this touch DT. **A 200 ms sleep is therefore not established for this binary's regulator path**; the observed path uses zero explicit per-rail sleeps. Regulator core/hardware ramp times are additional and unknown here.

`syna_dev_connect+0x2c..+0xb4` (0x6d8–0x760) invokes power-on, hardware reset, then `syna_tcm_detect_device(dev, 0, 1)` for auto-detection. `syna_spi_hw_reset+0x20..+0x80` writes raw assertion level, sleeps reset-active-ms, writes its inverse, sleeps reset-delay-ms: **LOW 2 ms, HIGH, wait 50 ms**. This uses raw GPIO access, not descriptor polarity inversion. Its GPIO setup initially requests reset deasserted (`syna_spi_probe`, around 0x169d8 onward); holding it low throughout ramp is a proposed conservative bare-metal sequence, not the exact Linux setup.

Probe `power_on` currently sets no-pull IRQ input and reset output low (`set_pin` writes DAT before CON), enables **LDO25M then LDO4M**, waits 200 ms plus 2 ms, releases reset high, and samples IRQ for 3000 ms. It preserves existing voltage bits when enabling rails; it does not itself set their voltages. This is more than a 2 ms reset pulse and reverses the build's rail order.

Proposed conservative chip sequence (inference, not a verified cure): keep CS inactive; hold reset low; enable AVDD 3.3 V then VDD 1.8 V; allow 200 ms settling; ensure >=2 ms asserted after rails stabilize; release reset; wait >=50 ms; attempt bounded protocol detection once AP routing is available. Keeping 200 ms here is conservative engineering judgment, not a claim that this binary sleeps that long.

The latest touch-plan.md section says the working transport order was **PMIC power/reset, then AOC start, then TBN attempt, then SPI reads**. Starting AOC before PMIC access previously prevented ACPM replies. Preserve that known dependency; the proposed chip sequence above is not an instruction to move AOC startup before power. Since reset currently precedes the lengthy AOC startup/routing changes, a fresh 2 ms reset and 50 ms wait after routing becomes usable is a reasonable proposed comparison, not a proven requirement.

### Startup message

The driver expects identify report code **0x10** after reset. V1 detection specifically accepts `A5 10 LL HH`, then retrieves the advertised payload (`syna_tcm_v1_detect+0xb0..+0xd0`, 0x5554–0x5574). V2's report dispatch also handles identify from reset/mode changes (linked reference `syna_tcm_v2_dispatch_report`; binary `syna_tcm_v2_dispatch_report` at 0x797c).

It is not established that every firmware mode will autonomously expose an identical startup packet or assert IRQ within a fixed upper bound. High IRQ for 3 s is an observation, not proof that supplies/reset are wrong. The subsequent IRQ drop after AOC pad writes in touch-plan.md is explicitly ambiguous there.

## 3. Framing, protocol selection, and the observed bytes

**Both versions are compiled in. The active version cannot be determined statically from these inputs.** `syna_dev_connect` passes version=0/auto. `syna_tcm_detect_device+0x44..+0xac` writes one byte `02` as an initial detection stimulus and reads exactly four bytes. It tries v2 detection first, then v1 (`+0xc8..+0xec`, 0x8f00–0x8f24). Thus even the driver's initial one-byte stimulus is not the complete ordinary IDENTIFY command framing below.

V1 detection checks first byte A5 (`syna_tcm_v1_detect+0x38..+0x40`, 0x54dc–0x54e4). V2 detection computes CRC6 over all 32 header bits, requires zero residue, then confirms by identifying the device (`syna_tcm_v2_detect`, 0x6a34 onward). Firmware mode=1 is application mode; it does **not** mean protocol v1. Logs saying only “TCM Fw mode: 0x01” cannot select the framing.

### V1

Logical unextended inbound message:

```
A5  code  payload_length_low  payload_length_high  payload[L]  5A
```

The 5A is protocol padding, not part of the payload length. Header is four bytes. **A new SPI read transaction for the remainder begins with `A5 03`**, followed by remaining data; do not treat the second transaction as payload starting at byte zero. `syna_tcm_v1_continued_read+0x1bc..+0x238` (0x58b8–0x5934) requests remaining bytes+2, checks byte 1=03, and copies from byte 2. This is a continuation *response*, not a host command to transmit.

This build also supports v1 **optional CRC and an extra trailing byte**. Initial detection initializes has_crc=1 and extra-byte=1 with 5A placeholders (`syna_tcm_v1_detect+0x94..+0xac`, 0x5538–0x5550); continued-read size computation includes them (0x5740–0x5770). On the initial identify path with no predictive bytes, it requests **L+7 bytes** in the second transaction: prefix 2 + L+5 bytes of payload/trailer. Its arithmetic adds L+3 for payload/padding/CRC, then extra-byte+1 (=2), then prefix 2. This is the driver's conservative discovery read size; do not infer a universal wire trailer layout solely from that size. `syna_tcm_v1_update_crc` reads candidate CRC at logical offset L+5 and extra byte at L+7. `syna_tcm_v1_detect+0x1b0..+0x204` disables CRC when the candidate is 5A5A and disables extra-byte support when its placeholder remains 5A. These extension details prevent claiming that the five-byte-overhead layout is the only layout this build can consume.

Ordinary low-level reads require A5, retry up to ten times with 5–10 ms sleeps (`syna_tcm_v1_read+0x84..+0xb4`, 0x5fd4–0x6004). The build can use predicted initial read lengths after learning prior message lengths (`syna_tcm_v1_read_message+0x68..+0xa8`); disable prediction in a minimal client.

V1 status code lives at byte 1, not byte 0. Verified binary handling: 01 is success; 03 as a fresh message is out-of-sync continued-read; FF is initialized/invalid state and is not an identify marker; 00 is handled as a status, not a data-ready marker (`syna_tcm_v1_read_message`, 0x47e8–0x4808, 0x4a74–0x4aa8, 0x4d20 onward). Common protocol terminology calls 00 IDLE and FF INVALID; complete status enums were not retrieved, so no additional numeric error map is asserted here.

A raw all-FF or all-00 read is not a valid v1 message because its first byte is not A5. Do not call either “chip not ready” without a valid header. It could instead be floating MISO, reset/unpowered chip, wrong routing/mode, or an invalid transaction; those explanations are guesses.

### V2

The linked reference `syna_tcm_v2_read`, `syna_tcm_v2_write`, `syna_tcm_v2_continued_read`, and `syna_tcm_v2_read_message` specifies: four-byte header `code, len_lo, len_hi, control`; control bit7 host, bit6 sequence, low6 CRC6. Header residue is zero. Payload-bearing packets append CRC16, initialized FFFF, high byte first. Header-only packets have no CRC16. Reading reports sends GET_REPORT; continuation uses ACK, rather than passive v1 stream reads. This reference description is corroborated by the build's functions below.

Binary corroboration: `syna_tcm_v2_write+0x28..+0x50` selects 4 bytes without payload or L+6 with payload; `+0x98..+0x1bc` constructs the header including sequence/CRC6; `syna_tcm_v2_read` at 0x82c8 checks framing; `syna_tcm_v2_read_message+0x40..+0x54` sends command **0x0A** (GET_REPORT); `syna_tcm_v2_execute_cmd_request+0x2f0..+0x30c` sends **0x07** (ACK) when needed before reading payload. The build's continuation logic has an optimization that can omit the first ACK (0x71f8–0x720c); a universal ACK-free sequence is not established.

For a header-only read request (N=0 payload), clock 4 bytes. For N>0 payload, clock N+6 and validate both CRCs. A length in an unvalidated header must never drive allocation or an unbounded read. The first header read is not simply followed by payload-only bytes under a fresh CS.

**D2 and F9:** No documented special meaning was found in the retrieved reference or inspected binary paths. In v1 they are invalid markers. In v2 the complete candidate headers `d2 ff ff ff` / `f9 ff ff ff` have CRC6 residues **0x25 / 0x2a**, respectively (not zero), and advertise implausible 65535-byte lengths. They are not valid identify/startup packets, and should not be decoded as named boot/status codes. All-FF v2 header residue is 06; an all-zero header also fails (do not confuse that with a valid IDLE header).

## 4. Minimal polled client, byte sequences and identify fields

These are protocols to implement later, **not actions performed by this analysis**. Transport prerequisites remain essential: AP owns the bus, chip is powered/reset released, correct mode, CS, working SCK/MISO/MOSI. Protocol changes cannot repair missing clocks or routing.

### Detection

After the power/reset sequence, optionally wait for raw IRQ low, but use a timeout and permit polling without IRQ: the build's initial detector calls its bus operations directly without first requiring an asserted IRQ (`syna_tcm_detect_device`, 0x8e78–0x8ee0). To reproduce its detector exactly, write `02` under one CS, release CS, then read 4 bytes under another CS. The exact initial write-to-read delay is not established by this function.

A simpler passive first attempt (proposed, not the exact driver detector) is a 4-byte read after reset. If A5, take the v1 branch. Otherwise verify CRC6 and confirm v2 with IDENTIFY; do not accept one accidental CRC match as proof. If neither validates, keep a bounded retry log of all four bytes and stop interpreting them as messages. Suggested transport filler for matching normal RX-only behavior is 00 (inference); retain an FF comparison only as a diagnostic, not a documented required filler.

### V1 baseline and this build's discovery extension

1. Read 4 bytes under CS: expect `A5 10 LL HH` for startup identify, or another valid status/report. L=LL|(HH<<8), bounded by the local buffer.
2. With CS high between transactions, wait 50–100 us. For unextended v1 read **L+3** bytes: expect `A5 03 payload[L] 5A`. Strip the two continuation bytes, retain L payload bytes. If payload spans transport chunks, every fresh transaction has its own A5 03 prefix. Do not re-read the original four-byte header as if it were the same packet.
3. To mirror this build's initial extension discovery instead, request **L+7** in step 2 and retain all trailer bytes for negotiation as described above. Implement FIFO streaming if that exceeds 64. This is verified driver sizing, not a claim that every chip transmits meaningful bytes throughout the overread.
4. Ordinary **unextended IDENTIFY request is exactly `02 00 00`**, command plus LE16 payload length zero, no A5 prefix or 5A suffix. `synaptics_touchcom_func_base.c:syna_tcm_identify`, binary `+0x50` (0x94fc), chooses command 02; `syna_tcm_v1_write+0xb4..+0x108` (0x6134–0x6188) constructs command/length and the zero-payload three-byte write. If CRC support is negotiated, this build can append two CRC bytes (`+0x10c..+0x134`); the unextended command is not asserted to work universally on CRC-enabled firmware.
5. Poll for valid messages and consume complete frames. A command response with status **01** carries the identify structure. An unsolicited report with code **10** also carries it. Do not require the requested reply to have code 10: `syna_tcm_identify` copies the response buffer (`+0x9c..+0xec`), whereas reset-report parsing is a distinct path.

Thus a fully universal byte-perfect v1 client including optional CRC/extra-byte negotiation is **partially unresolved** here. The baseline and the build's initial conservative read length are verified separately; do not silently combine them into an allegedly tested implementation.

### V2 exact header generation

A small CRC6 implementation inferred directly from the build's inlined CRC logic/table (table `.rodata+0xc668`, `syna_tcm_v2_write+0xa4..+0x1bc`; also `syna_tcm_v2_detect`) is:

```c
unsigned crc6_bits(const unsigned char *p, unsigned bits) {
    unsigned c = 0xfc;
    for (unsigned i = 0; i < bits; ++i) {
        c ^= ((p[i / 8] >> (7 - i % 8)) & 1) << 7;
        c = ((c << 1) ^ ((c & 0x80) ? 0x0c : 0)) & 0xff;
    }
    return c >> 2;
}
// h={command,len_lo,len_hi,(sequence & 1)<<6};
// h[3] |= crc6_bits(h,26); validate received header with crc6_bits(h,32)==0.
```

Calculated header-only commands (both sequence states, computed offline; not device-tested):

| Command | sequence 0 | sequence 1 |
|---|---|---|
| IDENTIFY 02 | `02 00 00 19` | `02 00 00 5A` |
| GET_REPORT 0A | `0A 00 00 17` | `0A 00 00 54` |
| ACK 07 | `07 00 00 0F` | `07 00 00 4C` |

Maintain one outgoing sequence toggle across commands; do not independently restart it for ACK. The binary increments its stored toggle before constructing a new header (`syna_tcm_v2_write+0x88..+0x98`); therefore the first value depends on initialization/history. The table exposes both possibilities instead of guessing a single universal first packet.

For IDENTIFY: send the appropriate four-byte command under CS, release CS, wait 50–100 us, read a four-byte response header. Validate CRC6 and length. If it advertises L>0, retrieve a framed continuation (normally ACK followed by a read of **L+6**, subject to the build's first-ACK optimization), validate header CRC6 and packet CRC16, take payload from offset 4. If the response acknowledges pending work, poll via GET_REPORT and handle intervening reports. Keep command success/status separate from report code 10. Detailed retry/status mapping and optimization initialization are unresolved; these prevent claiming a complete drop-in v2 client was recovered.

### Identification payload

Offsets are relative to payload, not bus frame:

| Offset | Size | Meaning / confidence |
|---|---|---|
| 0 | 1 | version, conventional TouchComm identification layout; field name not independently confirmed from retrieved header |
| 1 | 1 | firmware mode; verified load from id_info+1 |
| 2 | 16 | part number; strongly supported by part-number pointer id_info+2 and build ID starting +18; fixed-width bytes, do not assume NUL termination |
| 18 | 4 | build ID, little endian; verified |
| 22 | 2 | max write size, little endian; verified |
| 24 onward | variable | extensions; do not invent values or assume all firmware returns 48 bytes |

Binary evidence: `syna_tcm_v1_parse_idinfo+0x18..+0x28` copies min(L,48) to device+0x40; `+0x7c` reads build ID at device+0x52 (=payload+18); `+0x90` reads mode at +0x41 (=payload+1). `syna_tcm_v1_set_max_rw_size+0x1c` reads max write size at +0x56 (=payload+22). `syna_tcm_detect_device+0x25c..+0x268` passes device+0x42 as the part-number string. V2 has its own parse_idinfo symbol at 0x6e0c. Require L>=24 before decoding the listed complete fields. The version byte must not be used as a substitute for validated framing detection.

## 5. Reading coordinates later

Binary `syna_dev_interrupt_thread+0x3c..+0x6c` checks raw IRQ against configured active level, then calls `syna_tcm_get_event_data`. At `+0xe8..+0x110` (0x26c4–0x26ec), report **0x11** is passed to `syna_tcm_parse_touch_report`, then `syna_dev_report_input_events`. The interrupt handler also recognizes custom codes C2, C3, C5; C5 includes an LE16 touch-length prefix before invoking the touch parser (0x27e0–0x27fc). Do not parse every vendor report as ordinary 0x11 coordinates.

**Default report configuration is supplied by firmware, not recovered as a fixed array here.** `syna_dev_set_up_app_fw+0x30..+0x40` gets application information and calls `syna_tcm_preserve_touch_report_config` (0xa7c–0xa8c). That function sends **GET_TOUCH_REPORT_CONFIG 0x25**, no payload (`+0x40..+0x78`, 0xd708–0xd740), and saves the response. SET_TOUCH_REPORT_CONFIG is **0x26** (`syna_tcm_set_touch_report_config+0x13c`, 0xd650). This build's setup preserves the existing layout instead of installing a verified constant default table.

A later bare-metal coordinate client should: identify application mode, retrieve application info and report config, preserve the config bytes, parse the report's bit fields/loops according to that config, and handle object index/classification/X/Y fields as configured. The binary contains `syna_tcm_get_touch_data` (0xb084), `syna_tcm_parse_touch_report` (0xb16c), custom entity callback (0x1e98), and custom gesture callback (0x2008); setup registers both callbacks (0xaa0, 0xab8). Exact entity opcode table and per-object bit widths are **not recovered in this pass**, so an exact default coordinate parser cannot honestly be supplied.

## Remaining unknowns / completion boundary

SPI mode/ceiling, reset timing/polarity, build-specific rail order, normal RX-only transfer behavior, dual protocol detection, core framing and continuation, invalidity of the observed headers, baseline IDENTIFY bytes, identification offsets, and report-config retrieval are established above. Actual selected chip protocol, Samsung null-TX MOSI level and CS timing units, physical rail/routing cause, optional v1 extension negotiation details, v2 optimization initial state/status enums, and actual firmware touch configuration remain unresolved. No static evidence supports labeling D2/F9 as a named “not ready” code or guarantees that changing any single item will bring the chip up.
