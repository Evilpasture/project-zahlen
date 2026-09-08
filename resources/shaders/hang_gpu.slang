// resources/shaders/hang_gpu.slang
//
// Forced TDR: write through a protected low address so the GPU MMU faults.
// VK_KHR_shader_abort (OpAbortKHR) is optional device infrastructure and is
// not used here — shipping NVIDIA drivers still omit that extension.

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    // 0x100 is a protected, unmapped low-memory address.
    // Writing to it forces the GPU MMU to trigger an immediate hardware page fault.
    uint* invalidAddress = (uint*) 0x100ULL;

    invalidAddress[tid.x] = tid.x;
}
