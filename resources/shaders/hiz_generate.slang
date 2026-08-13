// resources/shaders/hiz_generate.slang

struct PushConstants {
    float rcpSrcWidth;
    float rcpSrcHeight;
    uint  srcWidth;
    uint  srcHeight;
    uint  isFirstPass;
};

[vk::push_constant] PushConstants pc;

Texture2D<float>   inDepth;
RWTexture2D<float> outDepth;
SamplerState       pointSampler;

[shader("compute")][numthreads(16, 16, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint2 dstSize;
    outDepth.GetDimensions(dstSize.x, dstSize.y);
    if (tid.x >= dstSize.x || tid.y >= dstSize.y)
        return;

    // First pass just copies Mip 0 directly from the standard Depth Buffer
    if (pc.isFirstPass != 0) {
        outDepth[tid.xy] = inDepth.Load(uint3(tid.x, tid.y, 0));
        return;
    }

    // Subsequent passes perform a conservative 2x2 max filter reduction
    float d0 = inDepth.Load(uint3(tid.x * 2, tid.y * 2, 0));
    float d1 = inDepth.Load(uint3(min(tid.x * 2 + 1, pc.srcWidth - 1), tid.y * 2, 0));
    float d2 = inDepth.Load(uint3(tid.x * 2, min(tid.y * 2 + 1, pc.srcHeight - 1), 0));
    float d3 = inDepth.Load(uint3(min(tid.x * 2 + 1, pc.srcWidth - 1), min(tid.y * 2 + 1, pc.srcHeight - 1), 0));

    outDepth[tid.xy] = max(max(d0, d1), max(d2, d3));
}
