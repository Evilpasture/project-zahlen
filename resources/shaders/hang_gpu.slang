
[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint* invalidAddress = (uint*) 0x100ULL;

    invalidAddress[tid.x] = tid.x;
}
