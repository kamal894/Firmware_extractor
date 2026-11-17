# Opaque red. ATEST updates coverage consumed by RT0 BLEND.
MOV.i32 r0, 0x3F800000
MOV.i32 r1, 0x0
MOV.i32 r2, 0x0
MOV.i32 r3, 0x3F800000
ATEST.discard @r60, r60, 0x3F800000, atest_datum.w0
BLEND.slot0.v4.f32.end @r0:r1:r2:r3, blend_descriptor_0.w0, r60, target:0x0
