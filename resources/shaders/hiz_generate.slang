
import gpu_push_constants;

[vk::push_constant]
HiZPushConstants pc;

Texture2D<float> inDepth;
RWTexture2D<float> outDepth;
SamplerState pointSampler;

[shader("compute")]
[numthreads(16, 16, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint2 dstSize;
    outDepth.GetDimensions(dstSize.x, dstSize.y);
    if (tid.x >= dstSize.x || tid.y >= dstSize.y)
        return;

    if (pc.isFirstPass != 0) {
        outDepth[tid.xy] = inDepth.Load(uint3(tid.x, tid.y, 0));
        return;
    }

    uint x0 = tid.x * 2u;
    uint y0 = tid.y * 2u;
    uint x1 = min(x0 + 1u, pc.srcWidth - 1u);
    uint y1 = min(y0 + 1u, pc.srcHeight - 1u);

    float d0 = inDepth.Load(uint3(x0, y0, 0)).r;
    float d1 = inDepth.Load(uint3(x1, y0, 0)).r;
    float d2 = inDepth.Load(uint3(x0, y1, 0)).r;
    float d3 = inDepth.Load(uint3(x1, y1, 0)).r;

    if (pc.reduceMin != 0u) {
        float minD = min(min(d0, d1), min(d2, d3));
        // Conservative odd-dimension fold (e.g. 1080 -> 540 -> 270 -> 135 -> 67)
        // so thin 1px silhouettes at odd row/column boundaries are never skipped.
        if ((pc.srcWidth & 1u) != 0u && tid.x + 1u == dstSize.x) {
            uint x2 = pc.srcWidth - 1u;
            minD    = min(minD, min(inDepth.Load(uint3(x2, y0, 0)).r, inDepth.Load(uint3(x2, y1, 0)).r));
        }
        if ((pc.srcHeight & 1u) != 0u && tid.y + 1u == dstSize.y) {
            uint y2 = pc.srcHeight - 1u;
            minD    = min(minD, min(inDepth.Load(uint3(x0, y2, 0)).r, inDepth.Load(uint3(x1, y2, 0)).r));
            if ((pc.srcWidth & 1u) != 0u && tid.x + 1u == dstSize.x) {
                minD = min(minD, inDepth.Load(uint3(pc.srcWidth - 1u, y2, 0)).r);
            }
        }
        outDepth[tid.xy] = minD;
    } else {
        outDepth[tid.xy] = max(max(d0, d1), max(d2, d3));
    }
}
