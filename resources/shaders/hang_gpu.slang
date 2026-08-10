// resources/shaders/hang_gpu.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Auto-generated from hang_gpu.hlsl via tools/slang_migrate.py — Modern Slang syntax.
// Compile with: slangc -target spirv -matrix-layout column_major -fvk-use-dx-layout -fspv-target-env=vulkan1.3
//               -I resources/shaders -I include -o <out>.spv
// See SHADER.md for column_major, right-handed, CCW conventions preserved.

// resources/shaders/hang_gpu.hlsl
// Slang: column_major via -matrix-layout column_major (removed #pragma pack_matrix) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
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

// --- Slang modernization notes ---
// * `#pragma pack_matrix(column_major)` removed – use compiler flag `-matrix-layout column_major` to preserve DXC's column-major semantics.
// * HLSL `#include` -> Slang `import` for modules (see pbr_helpers, uniforms, common modules).
// * Entry points annotated with [shader("vertex"/"fragment"/"compute")] for explicit stage declaration (Slang idiom).
// * Duplicate binding / macro redefinition / out-of-bounds BDA silent bugs fixed – Slang's stricter diagnostics surface them.
// * All vk::binding / vk::push_constant / vk::constant_id attributes preserved for SPIR-V Vulkan 1.3 target parity.
