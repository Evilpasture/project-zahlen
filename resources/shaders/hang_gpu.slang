// resources/shaders/hang_gpu.slang

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    // 0x100 is a protected, unmapped low-memory address.
    // Writing to it forces the GPU MMU to trigger an immediate hardware page fault.
    uint* invalidAddress = (uint*) 0x100ULL;

    invalidAddress[tid.x] = tid.x;
}
