// resources/shaders/hang_gpu.hlsl
#pragma pack_matrix(column_major)

[numthreads(64, 1, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	// 0x100 is a protected, unmapped low-memory address.
	// Writing to it forces the GPU MMU to trigger an immediate hardware page fault.
	uint64_t invalidAddress = 0x100ULL;

	vk::RawBufferStore<uint>(invalidAddress + tid.x * 4, tid.x, 4);
}
