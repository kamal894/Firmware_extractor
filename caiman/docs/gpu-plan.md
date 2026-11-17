# GPU: Mali-G715 on caiman

Android: `Mali-G715 7 cores r1p3 0x0B080A02` (sysfs gpuinfo); CSF firmware files copied to
`~/caiman-work/gpu/` (`mali_csffw-*.bin`; the driver logs "Loading Mali firmware 0x2080000 ...
Firmware boot completed"). DT `mali@1F000000` (0x480000), interrupts JOB 0x15b, MMU 0x15c, GPU 0x15a,
power domains pd-g3d@15461A80 ("top", switched through an SMC) and pd-embedded_g3d@15461C80 ("cores"),
S2MPU in front (`s2mpus`).

## Step 1: read only (2026-11-17, PROBE_GPU=1, docs/probe-log-gpu1.txt)

- Both power domains are **on** when the bootloader hands over (+0 = 1, +4 status = 1).
- GPU_ID 0xb8a21030: arch 11.8.10, product 2, r1p3 (the same GPU Android reports).
- SHADER_PRESENT 0x01110055 (7 cores), TILER 1, L2 1, AS_PRESENT 0xff, CSF_ID 0x04100412,
  MMU_FEATURES 0x2830 (VA 48, PA 40 bits).
- L2, shaders and the MCU are off (READY 0, MCU_STATUS 0): nothing has started it.

## Next

2. L2 and shader cores on (L2_PWRON / SHADER_PWRON, watch READY), still no firmware.
3. GPU MMU address space 0 page tables for the MCU, load `mali_csffw` sections as kbase does, MCU on,
   wait for "firmware booted" (GLB interface).
4. Simple jobs (a fill, a copy), then a triangle; then a Metal back end (HackiOS's hakios-metal, for
   Adreno today) with a Valhall shader compiler.
