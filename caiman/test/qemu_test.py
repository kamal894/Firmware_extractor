#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
End-to-end test of caiman-probe in QEMU (aarch64 `virt`, entered at EL2,
PSCI over SMC, like the Pixel).

The test device tree carries caiman-shaped nodes (ramoops in a
reserved-memory with #size-cells = 1, a bootloader log region, a DECON
node with a handover memory-region, DPP nodes). The display "registers"
and the bootloader log are RAM filled in before boot. After the probe
reboots, QEMU pauses instead of resetting; the test reads RAM back over
the monitor and decodes the ramoops console zone exactly like Linux.

Scenarios:  default | el1 (entered at EL1, PSCI over HVC) |
            xnu (LOADER=1: HackiOS's prebuilt kernel started by the loader, its log from pstore;
                 needs HAKIOS_KERNEL, default the hackios-xnu-kernel checkout's prebuilt/) |
            hello (the same with HackiOS's raw-syscall `hello` as the RAM disk: process 1 in user
                   space, then the kernel's panic when it exits; needs HAKIOS_HELLO) |
            bare-dt (no caiman nodes in the DT: built-in fallbacks) |
            ufs (PROBE_UFS=2: the UFS look and read test, against a fake controller that never
                 completes a request: exercises the timeout path) |
            paint (PROBE_PAINT=1) | crash (PROBE_CRASHTEST=1)

Needs: qemu-system-aarch64, dtc, clang, ld.lld, llvm-objcopy.
Usage:  python3 caiman/test/qemu_test.py [--keep] [scenario ...]
"""
import argparse
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.join(HERE, "..", "probe")
sys.path.insert(0, os.path.join(HERE, "..", "tools"))
import decode_ramoops  # noqa: E402

RAM = "2G"
MMIO_BASE = 0x50000000          # fake DPU block (DECON main/win/wincon, DPUF0/1 DMA)
BLDR_BASE = 0x50400000
FB_BASE = 0x58000000
RAMOOPS_BASE = 0x60000000
W, H = 1280, 2856

DT_EXTRA = f"""
/ {{
	reserved-memory {{
		#address-cells = <2>;
		#size-cells = <1>;
		ranges;

		ramoops_mem@{RAMOOPS_BASE:x} {{
			compatible = "ramoops";
			reg = <0 0x{RAMOOPS_BASE:x} 0x400000>;
			console-size = <0x200000>;
			pmsg-size = <0x200000>;
		}};
		bldr_log_reserved@{BLDR_BASE:x} {{
			reg = <0 0x{BLDR_BASE:x} 0x10000>;
			no-map;
		}};
		splash: splash@{FB_BASE:x} {{
			reg = <0 0x{FB_BASE:x} 0x{W * H * 4:x}>;
		}};
	}};

	drmdecon@{MMIO_BASE:x} {{
		compatible = "samsung,exynos-decon";
		/* QEMU's root node uses 2 address + 2 size cells (caiman's uses 2 + 1) */
		reg = <0 0x{MMIO_BASE:x} 0 0x6000>, <0 0x{MMIO_BASE + 0x10000:x} 0 0xe000>,
		      <0 0x{MMIO_BASE + 0x20000:x} 0 0xe000>;
		reg-names = "main", "win", "wincon";
		decon,id = <0>;
		memory-region = <&splash>;
	}};
	drmdpp@{MMIO_BASE + 0x102000:x} {{
		compatible = "samsung,exynos-dpp";
		reg = <0 0x{MMIO_BASE + 0x102000:x} 0 0x1000>;
		reg-names = "dma";
		dpp,id = <2>;
	}};
	drmdpp@{MMIO_BASE + 0x201000:x} {{
		compatible = "samsung,exynos-dpp";
		reg = <0 0x{MMIO_BASE + 0x201000:x} 0 0x1000>;
		reg-names = "dma";
		dpp,id = <8>;
	}};
}};
"""


def fake_mmio():
    """4 MiB image of the DPU (and UFS) register space as the bootloader would leave it."""
    m = bytearray(0x400000)

    def w32(off, v):
        struct.pack_into("<I", m, off, v)

    # DECON0 main as the Pixel 9 Pro's bootloader leaves it (first hardware run):
    # command mode, enabled, running, hw (TE) trigger masked; shadow copy at +0x800
    w32(0x0000, 0x09865000)
    w32(0x0004, 1234)
    w32(0x0020, 0x113)
    w32(0x0820, 0x113)
    w32(0x0030, 0x10)
    # cluster-0 watchdog: running, reset on, prescaler 256, /128 -> 750 Hz
    w32(0x280000, 0xff39)
    w32(0x280004, 0x8000)
    w32(0x280008, 0x4000)
    # window 0: chmap 9 -> dpp 8 (DPUF1), full screen
    w32(0x20000 + 0 * 0x1000, (9 << 4) | 1)
    w32(0x10000 + 0 * 0x1000 + 0xC, 0)
    w32(0x10000 + 0 * 0x1000 + 0x10, ((H - 1) << 16) | (W - 1))
    # window 1: solid-colour window (no DMA) -> must be skipped
    w32(0x20000 + 1 * 0x1000, (0 << 4) | 2 | 1)
    # window 2: chmap 2 -> dpp 2 (DPUF0), small overlay -> must lose to window 0
    w32(0x20000 + 2 * 0x1000, (2 << 4) | 1)

    def rdma(base, addr, w, h, fmt):
        for shd in (0, 0x400):
            w32(base + shd + 0x00, 1)
            w32(base + shd + 0x08, fmt << 8)
            w32(base + shd + 0x10, w)
            w32(base + shd + 0x14, h)
            w32(base + shd + 0x1C, (h << 16) | w)
            w32(base + shd + 0x40, addr)
            w32(base + shd + 0x50, 0)

    # UFS host controller at +0x300000 (PLAT_UFS_HCI on qemu), running, with a request list at
    # +0x380000 holding one finished READ(10) as a bootloader leaves it
    w32(0x300000, 0x0107001f)                 # CAP: 32 slots, 8 task slots, 64-bit
    w32(0x300008, 0x00000300)                 # VER 3.0
    w32(0x300030, 0x0000010f)                 # HCS: DP, UTRLRDY, UTMRLRDY, UCRDY
    w32(0x300034, 1)                          # HCE
    w32(0x300050, MMIO_BASE + 0x380000)       # UTRLBA
    w32(0x300060, 1)                          # UTRLRSR
    w32(0x300090, 0x17)                       # UICCMD: DME_HIBER_ENTER, as the Pixel's bootloader leaves it
    w32(0x30110c, 0xb2)                       # vendor 1US_TO_CNT_VAL: 178 (caiman's bootloader)
    w32(0x320044, 90)                         # UniPro period value the bootloader's CAL wrote
    w32(0x313ec0, 1)                          # PMU ufs-phy-iso: bypassed
    w32(0x319000, 0x10)                       # gpp0 CON: pin 1 output
    w32(0x319004, 0)                          # gpp0 DAT: VCC off, as after "shutdown complete"
    w32(0x3011b0, 0x17)                       # vendor CLKSTOP_CTRL: internal clocks stopped (Hibern8)
    w32(0x3011b4, 0xde0)                      # vendor FORCE_HCS: CLK_STOP_CTRL_EN_ALL
    w32(0x323080, 2)                          # UniPro PA_AvailTxDataLanes (PLAT_UFS_UNIPRO on qemu)
    w32(0x323184, 2)                          # PA_ConnectedTxDataLanes
    w32(0x323204, 2)                          # PA_ConnectedRxDataLanes
    w32(0x304000, 0x51)                       # PHY common 0x000 after the CAL's post_h8_enter
    for lane in (0, 1):
        w32(0x304000 + 0x800 * lane + 0x9f4, 0x08)
        w32(0x304000 + 0x800 * lane + 0xa00, 0x3a)
        w32(0x304000 + 0x800 * lane + 0xb64, 0x33)
    w32(0x380000, 0x21000000)                 # UTRD: command type UFS storage, data in
    w32(0x380010, MMIO_BASE + 0x381000)       # UCD
    w32(0x380018, (0x80 << 16) | 0x20)        # response @ dword 0x80, 0x20 dwords
    w32(0x38001c, (0x88 << 16) | 1)           # PRDT @ dword 0x88, 1 entry
    w32(0x381000, 0x00000001)                 # command UPIU: transaction code 0x01
    w32(0x381220, MMIO_BASE + 0x390000)       # PRDT entry 0: data address
    w32(0x38122c, 0xfff)                      # ... byte count - 1
    rdma(0x201000, FB_BASE, W, H, 0)          # dpp 8 = DPUF1 L8
    rdma(0x102000, 0x5f000000, 100, 100, 7)   # dpp 2 = DPUF0 L2
    return bytes(m)


def fake_bldr_log():
    text = (
        b"\x7fBLDRLOG\x01\x00\x00\x00\x10\x00\x00\x00"
        b"[    0.012] fake-abl: hello from the test bootloader\n"
        b"[    0.345] fake-abl: display: decon0 video mode, fb 0x58000000 1280x2856\n"
        b"\x00\x00\x01\x02"
        b"[    1.000] fake-abl: starting kernel\n"
    )
    return text + b"\0" * (0x10000 - len(text))


class Monitor:
    def __init__(self, path):
        for _ in range(100):
            try:
                self.s = socket.socket(socket.AF_UNIX)
                self.s.connect(path)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise RuntimeError("cannot connect to QEMU monitor")
        self.s.settimeout(5)
        self._read_until(b"(qemu)")

    def _read_until(self, token):
        buf = b""
        while token not in buf:
            chunk = self.s.recv(4096)
            if not chunk:
                break
            buf += chunk
        return buf.decode("utf-8", "replace")

    def cmd(self, c):
        self.s.sendall(c.encode() + b"\n")
        return self._read_until(b"(qemu)")


def run(cmd, **kw):
    r = subprocess.run(cmd, **kw)
    if r.returncode:
        sys.exit(f"command failed: {' '.join(cmd)}")
    return r


def build(scenario, work):
    out = f"out/qemu-{scenario}"
    flags = {"default": [], "el1": [], "bare-dt": [], "paint": ["PROBE_PAINT=1"],
             "crash": ["PROBE_CRASHTEST=1"],
             "xnu": ["LOADER=1", "PROBE_XNU_SECONDS=40"],
             "hello": ["LOADER=1", "PROBE_XNU_SECONDS=40"],
             "ufs": ["PROBE_UFS=2"]}[scenario]
    run(["make", "-s", "-C", PROBE, "PLATFORM=qemu", f"OUT={out}", "PROBE_FINISH_DELAY_MS=100"]
        + flags, stdout=subprocess.DEVNULL)
    image = os.path.join(PROBE, out, "probe.Image")
    if scenario not in XNU_SCENARIOS:
        return image
    combined = os.path.join(work, "xnu.Image")
    extra = ["--ramdisk", HAKIOS_HELLO] if scenario == "hello" else []
    run([sys.executable, os.path.join(HERE, "..", "tools", "mkxnuimg.py"), "--loader", image,
         "--kernel", HAKIOS_KERNEL, "--kernel-sha256", HAKIOS_KERNEL_SHA256, "--cmdline=-v",
         *extra, "--out-image", combined], stdout=subprocess.DEVNULL)
    return combined


HAKIOS_KERNEL = os.environ.get(
    "HAKIOS_KERNEL", "/home/user/oosexgang/hackios-xnu-kernel/prebuilt/hakios-kernel")
HAKIOS_KERNEL_SHA256 = "03e794e8e1d389a6abb9bcd2431eaccf9747271ff8c1234922225718c70441e9"
HAKIOS_HELLO = os.environ.get(
    "HAKIOS_HELLO", "/home/user/oosexgang/hackios-xnu-kernel/prebuilt/userland/hello")
XNU_SCENARIOS = ("xnu", "hello")


def machine(scenario):
    # el1: no virtualization extensions -> entered at EL1, PSCI over HVC
    if scenario == "el1":
        return "virt"
    # xnu, hello: GICv3 as on the Pixel (Tensor G4), for HackiOS's kernel
    return "virt,virtualization=on,gic-version=3" if scenario in XNU_SCENARIOS else "virt,virtualization=on"


def qemu_size(scenario):
    # xnu: XNU's 4 GiB window at 0x80000000 and a second core for the watcher. Two NUMA nodes
    # make QEMU list RAM as two adjacent ranges with XNU's window across the seam, as the Pixel
    # lists its high bank as seven adjacent 2 GiB ranges.
    if scenario in XNU_SCENARIOS:
        return ["-smp", "2", "-m", "6G",
                "-object", "memory-backend-ram,id=m0,size=3G", "-numa", "node,memdev=m0",
                "-object", "memory-backend-ram,id=m1,size=3G", "-numa", "node,memdev=m1"]
    return ["-m", RAM]


def make_dtb(work, scenario):
    base = os.path.join(work, f"virt-{scenario}.dtb")
    run(["qemu-system-aarch64", "-machine", f"{machine(scenario)},dumpdtb={base}",
         "-cpu", "cortex-a76", *qemu_size(scenario), "-display", "none", "-nic", "none"], stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL)
    dts = subprocess.run(["dtc", "-q", "-I", "dtb", "-O", "dts", base], check=True,
                         stdout=subprocess.PIPE).stdout.decode()
    src = os.path.join(work, f"test-{scenario}.dts")
    # bare-dt: QEMU's own tree only, so every address comes from the built-in fallbacks
    open(src, "w").write(dts if scenario == "bare-dt" else dts + DT_EXTRA)
    dtb = os.path.join(work, f"test-{scenario}.dtb")
    run(["dtc", "-q", "-I", "dts", "-O", "dtb", "-o", dtb, src])
    return dtb


def boot(image, dtb, work, scenario):
    for name, data in (("mmio.bin", fake_mmio()), ("bldr.bin", fake_bldr_log())):
        open(os.path.join(work, name), "wb").write(data)
    sock = os.path.join(work, "mon.sock")
    uart = os.path.join(work, "uart.log")
    q = subprocess.Popen([
        "qemu-system-aarch64", "-machine", machine(scenario), "-cpu", "cortex-a76",
        *qemu_size(scenario), "-display", "none", "-nic", "none", "-serial", f"file:{uart}",
        "-monitor", f"unix:{sock},server,nowait",
        "-kernel", image, "-dtb", dtb, "-append", "probe-test bootargs",
        "-device", f"loader,file={work}/mmio.bin,addr=0x{MMIO_BASE:x},force-raw=on",
        "-device", f"loader,file={work}/bldr.bin,addr=0x{BLDR_BASE:x},force-raw=on",
        "-action", "reboot=shutdown", "-action", "shutdown=pause",
    ])
    try:
        mon = Monitor(sock)
        deadline = time.time() + (240 if scenario in XNU_SCENARIOS else 60)
        while time.time() < deadline:
            st = mon.cmd("info status")
            if "paused" in st or "shutdown" in st:
                break
            time.sleep(0.2)
        else:
            raise RuntimeError("probe never asked for a reset (hang?)")
        dump = os.path.join(work, "ramoops.bin")
        fbd = os.path.join(work, "fb.bin")
        mon.cmd(f'pmemsave 0x{RAMOOPS_BASE:x} 0x400000 "{dump}"')
        mon.cmd(f'pmemsave 0x{FB_BASE:x} 0x{W * 4:x} "{fbd}"')
        mon.cmd("quit")
    finally:
        try:
            q.wait(timeout=10)
        except subprocess.TimeoutExpired:
            q.kill()
    log = decode_ramoops.decode(open(dump, "rb").read())
    return log, open(uart, errors="replace").read(), open(fbd, "rb").read()


def expect(log, needles, label):
    bad = [n for n in needles if n not in log]
    for n in needles:
        print(f"    [{'ok' if n not in bad else 'MISSING'}] {n}")
    if bad:
        raise AssertionError(f"{label}: {len(bad)} expected line(s) missing")


COMMON = [
    "=== caiman-probe",
    "Cortex-A76",
    "dt: /chosen bootargs: probe-test bootargs",
    "ramoops: DT ramoops_mem@60000000: 0x60000000 size 0x400000 record 0x0 console 0x200000",
    "matches the built-in default",
    "dt: rsv bldr_log_reserved@50400000",
    "uart: hello over serial from caiman-probe",
    "selftest: guarded read of unmapped 0xe000000 faulted and was skipped (ok)",
    "fake-abl: display: decon0 video mode, fb 0x58000000 1280x2856",
    "fake-abl: starting kernel",
    "display: DT handover memory-region -> splash@58000000: 0x58000000 size 0xdf2000",
    "display: mode=COMMAND enabled=yes",
    "wdt: WTCON=0x0000ff39 WTDAT=0x00008000 WTCNT=0x00004000: RUNNING, reset on expiry ON",
    "period 43690 ms, 21845 ms left",
    "watchdog     : RUNNING (bootloader left it armed), period 43690 ms",
    "display: window 0 ON: WINCON=0x00000091 chmap=9 -> dpp 8",
    "[solid colour, no DMA]",
    "display:   reading RDMA @ 0x50201000",
    "display:   reading RDMA @ 0x50102000",
    "display: FRAME BUFFER: 0x58000000  1280x2856  BGRA8888  stride 5120 bytes  (dpp 8)",
    "display: DT handover region CONTAINS the register address",
    "=== SUMMARY ===",
    "aborted reads: 1",
    "display: DECON0 main=0x50000000 win=0x50010000 wincon=0x50020000 (14 windows)",
    "pos (0,0)-(1279,2855)",
]

EL2 = ["cpu: CurrentEL=2", "cpu: SCTLR_EL2=", "dt: psci method=smc", "psci: conduit=smc version=1."]

SCENARIOS = {
    "default": COMMON + EL2 + ["finish: done; rebooting", "=== caiman-probe end ==="],
    "el1": COMMON + ["cpu: CurrentEL=1", "cpu: SCTLR_EL1=", "dt: psci method=hvc",
                     "psci: conduit=hvc version=1.", "finish: done; rebooting",
                     "=== caiman-probe end ==="],
    "bare-dt": EL2 + [
        "=== caiman-probe", "ramoops: no compatible=\"ramoops\" node in DT",
        "uart: hello over serial from caiman-probe",
        "--- bootloader log @ 0x50400000 (0x10000 bytes) ---", "fake-abl: starting kernel",
        "display: DECON0 main=0x50000000 win=0x50010000 wincon=0x50020000 (14 windows)",
        "display:   reading RDMA @ 0x50201000", "display:   reading RDMA @ 0x50102000",
        "display: FRAME BUFFER: 0x58000000  1280x2856  BGRA8888  stride 5120 bytes  (dpp 8)",
        "fb (DT)      : no handover memory-region", "=== caiman-probe end ===",
    ],
    "xnu": COMMON + EL2 + [
        "dt: memory 0x40000000 - 0x100000000", "dt: memory 0x100000000 - 0x1c0000000",
        "xnu: kernel 16809112 bytes, ramdisk 0 bytes, XNU memory 0x80000000 + 0x100000000",
        "xnu: console -> loader_log (vmapple_uart transmit patched, receive off)",
        "loader_log found", "xnu: boot-args: debug=0x8 serial=3 cpus=1 -v",
        "watcher: PSCI CPU_ON(0x1) = 0", "watcher: running on MPIDR 0x80000001 at EL2",
        "=== XNU after 40 s (last look): loader_log",
        "Darwin Kernel Version 22.5.0", "DEVELOPMENT_ARM64_BCM2837",
        "GenericARMCPU: cpu 0 (phys id 0x0) registered",
        "MiniIOMobileFramebuffer: display 1125x2436 on a panel of 1280x2856",
        "!BSD", "Waiting on <dict",
        "watcher: 40 s are up; resetting the phone",
    ],
    "hello": COMMON + EL2 + [
        "xnu: kernel 16809112 bytes, ramdisk 16480 bytes",
        "xnu: boot-args: debug=0x8 serial=3 cpus=1 rd=md0 -v",
        "watcher: running on MPIDR 0x80000001 at EL2",
        "Darwin Kernel Version 22.5.0",
        "BSD root: md0, major 1, minor 0",
        "load_init_program: attempting to load /sbin/launchd",
        "Hello from user space on Darwin!",
        "I am process",
        "panic(cpu 0 caller", "initproc failed to start",
        "=== panic: ",
        "screen: 1280x2856 at 0x58000000, 64x48 cells", "cells lit",
        "watcher: 40 s are up; resetting the phone",
    ],
    "ufs": COMMON + EL2 + [
        "--- ufs (read-only) ---",
        "ufs: CAP 0x0107001f (32 transfer slots, 8 task slots, 64-bit yes) VER 0x00000300",
        "ufs: HCE 0x00000001 (enabled)  HCS 0x0000010f: device present, UTRL ready, UTMRL ready, UIC cmd ready, power mode 1",
        "ufs: transfer request list (the bootloader's) at 0x50380000, 32 slots",
        "ufs:   slot 0: OCS 0x00, UCD 0x50381000, response @+128 len 32, PRDT @+136 x1",
        "ufs:   command UPIU @ 0x50381000: 00000001",
        "ufs:   PRDT @dw @ 0x50381220: 50390000 00000000 00000000 00000fff",
        "ufs: verdict: UP: the bootloader left the link running",
        "--- ufs cold start (the bootloader left the device powered down) ---",
        "ufs: bootloader's timing: 1us = 178 UniPro clocks, UniPro period value 90",
        "ufs: PHY isolation register (PMU) 0x00000001 (bypassed: PHY powered)",
        "ufs: device VCC (gpp0-1): CON 0x00000010 (function 1) DAT 0x00000000: OFF",
        "ufs: device VCC switched on: DAT 0x00000002",
        "ufs: try 1",
        # (fake registers are RAM: the software reset bit never clears, the UIC command seems to
        # complete because IS keeps what is written, and the request list never runs)
        "ufs:   host software reset did not finish",
        "ufs:   host enabled: HCE 0x00000001",
        "ufs:   pre_link: lane 1 PHY calibration NOT done (0xc74 = 0x00 after 100 polls)",
        "ufs:   pre_link: 57 entries applied (mclk 178500000 Hz, period 6 ns, UniPro period 90, line reset 178500/571200 ticks)",
        "ufs:   DME_LINKSTARTUP: answered, result 0 (0 = success)",
        "ufs:   link up: connected lanes tx 2 rx 2",
        "ufs:   post_link: 5 entries applied",
        "ufs:   NOP OUT: FAILED after 5 tries",
        "ufs: cold start failed after link start-up",
        "=== caiman-probe end ===",
    ],
    "paint": COMMON + EL2 + ["paint: colour bars into 0x58000000 (1280x2856, stride 5120)", "paint: done",
                       "kick: TRIG_CON=0x00000010 (hw trigger, masked) FRAME_COUNT=1234",
                       "kick: TRIG_CON now 0x00000001;",
                       "=== caiman-probe end ==="],
    "crash": COMMON + EL2 + ["crashtest: executing an undefined instruction",
                       "!!! EXCEPTION: sync from current EL (SPx) at EL2",
                       "EC=0x00 (unknown/undefined instruction)",
                       "finish: exception; rebooting", "=== caiman-probe end ==="],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("scenarios", nargs="*", default=list(SCENARIOS))
    ap.add_argument("--keep", action="store_true", help="keep the work directory")
    ap.add_argument("--show", action="store_true", help="print the decoded log")
    a = ap.parse_args()
    for tool in ("qemu-system-aarch64", "dtc", "clang", "ld.lld"):
        if not shutil.which(tool):
            sys.exit(f"missing tool: {tool}")

    work = tempfile.mkdtemp(prefix="caiman-probe-test-")
    failed = 0
    try:
        for sc in a.scenarios:
            print(f"== scenario: {sc}")
            d = os.path.join(work, sc)
            os.makedirs(d)
            if sc in XNU_SCENARIOS and not (os.path.exists(HAKIOS_KERNEL) and
                                            (sc != "hello" or os.path.exists(HAKIOS_HELLO))):
                print(f"   SKIP: no HackiOS kernel at {HAKIOS_KERNEL} (set HAKIOS_KERNEL)")
                continue
            log, uart, fb = boot(build(sc, d), make_dtb(work, sc), d, sc)
            open(os.path.join(d, "console-ramoops.txt"), "w").write(log)
            if a.show:
                print(log)
            try:
                expect(log, SCENARIOS[sc], sc)
                # UART got the replay of the early log plus the rest (bootloader log muted)
                expect(uart, ["=== caiman-probe", "uart: hello over serial", "=== SUMMARY ==="],
                       sc + " uart")
                if sc in XNU_SCENARIOS and "Darwin Kernel Version" in uart:
                    raise AssertionError("XNU's console reached the UART: it must go only to loader_log")
                if "fake-abl: starting kernel" in uart:
                    raise AssertionError("bootloader log should not be sent to the UART")
                first = struct.unpack_from("<I", fb, 0)[0]
                if sc == "paint" and first != 0xFFFFFFFF:
                    raise AssertionError(f"frame buffer not painted (first pixel 0x{first:08x})")
                if sc not in ("paint", "xnu", "hello") and first != 0:
                    raise AssertionError("frame buffer modified without PROBE_PAINT")
                print(f"   PASS ({len(log)} bytes of log)")
            except AssertionError as e:
                failed += 1
                print(f"   FAIL: {e}\n   log: {os.path.join(d, 'console-ramoops.txt')}")
                a.keep = True
    finally:
        if a.keep:
            print(f"work directory kept: {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
