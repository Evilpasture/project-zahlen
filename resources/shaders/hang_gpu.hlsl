// resources/shaders/hang_gpu.hlsl
#pragma pack_matrix(column_major) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

// SLANG SAFETY: Unconditional BDA store to 0x100 is a deliberate GPU hang test.
// Modern Slang requires explicit opt-in via -D ENABLE_HANG_TEST.
// Without the define, this shader becomes a safe no-op to avoid silent TDRs that DXC hides.
#ifndef ENABLE_HANG_TEST
[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    // Safe no-op: hang test disabled (compile with -D ENABLE_HANG_TEST to enable)
    return;
}
#else
[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	// 0x100 is a protected, unmapped low-memory address.
	// Writing to it forces the GPU MMU to trigger an immediate hardware page fault.
	uint64_t invalidAddress = 0x100ULL;

	vk::RawBufferStore<uint>(invalidAddress + tid.x * 4, tid.x, 4);
}
#endif
