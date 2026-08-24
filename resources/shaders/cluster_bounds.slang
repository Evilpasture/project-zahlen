// File: resources/shaders/cluster_bounds.slang

import cluster_grid;
import cluster_math;
import uniforms;

// Host reflects IDs 1000-1002 as this kernel's logical dispatch domain.
namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Cluster.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Cluster.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Cluster.Depth;
}

RWStructuredBuffer<ClusterBounds> out_Bounds;
ConstantBuffer<FrameUniforms> frame;

float4 UnprojectView(float4 coord) {
    // View-space bounds: cluster culling tests lights[i].positionView.
    float4 res = mul(frame.invProj, coord);
    return res / res.w;
}

[numthreads(Cluster.Width, Cluster.Height, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= Dispatch.SizeX || tid.y >= Dispatch.SizeY || tid.z >= Dispatch.SizeZ)
        return;
    uint cIdx = ClusterIndex(tid.x, tid.y, tid.z);

    float2 ts = float2(2.0f / float(Cluster.Width), 2.0f / float(Cluster.Height));

    // UN-FLIPPED: NDC Y coordinates now map natively to Vulkan Y-down
    // tid.y = 0 maps to the top and Cluster.Height maps to the bottom.
    float4 ndc[4] = { float4(-1.0f + tid.x * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f), float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
                      float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f),
                      float4(-1.0f + tid.x * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f) };

    float3 pNear[4];
    float3 pFar[4];
    for (int i = 0; i < 4; ++i) {
        pNear[i] = UnprojectView(float4(ndc[i].xy, 0.0f, 1.0f)).xyz;
        pFar[i]  = UnprojectView(float4(ndc[i].xy, 1.0f, 1.0f)).xyz;
    }

    float n     = Cluster.Near;
    float f     = Cluster.Far;
    float sNear = ClusterSliceNear(tid.z);
    float sFar  = ClusterSliceFar(tid.z);

    float tNear = (sNear - n) / (f - n);
    float tFar  = (sFar - n) / (f - n);

    float3 pMin = float3(1e30f, 1e30f, 1e30f);
    float3 pMax = float3(-1e30f, -1e30f, -1e30f);
    for (int j = 0; j < 4; ++j) {
        float3 ptNear = lerp(pNear[j], pFar[j], tNear);
        float3 ptFar  = lerp(pNear[j], pFar[j], tFar);
        pMin          = min(pMin, min(ptNear, ptFar));
        pMax          = max(pMax, max(ptNear, ptFar));
    }

    out_Bounds[cIdx].minPoint = float4(pMin, 1.0f);
    out_Bounds[cIdx].maxPoint = float4(pMax, 1.0f);
}
