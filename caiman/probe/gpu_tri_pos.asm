# Vertex ID r60 selects screen positions. Internal ID r59 addresses the output.
IADD_IMM.i32 r0, 0x0, #0x41000000
IADD_IMM.i32 r1, 0x0, #0x41000000
IADD_IMM.i32 r6, 0x0, #0x42600000
IADD_IMM.i32 r7, 0x0, #0x42000000
IADD_IMM.i32 r8, 0x0, #0x1
IADD_IMM.i32 r9, 0x0, #0x2
CSEL.u32.eq r0, r60, r8, r6, r0
CSEL.u32.eq r0, r60, r9, r7, r0
CSEL.u32.eq r1, r60, r9, r6, r1
IADD_IMM.i32 r2, 0x0, #0x3f000000
MOV.i32 r3, 0x3F800000
LEA_BUF_IMM.slot0.wait0 @r4:r5, r59, table:0x3d, index:0x0
STORE.i128.istream.slot0.end @r0:r1:r2:r3, [r4^:r5^], offset:0
