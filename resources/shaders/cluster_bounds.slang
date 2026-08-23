// File: resources/shaders/cluster_bounds.hlsl

import cluster_grid;
import uniforms;

struct ClusterBounds {
    float4 minPoint;
    float4 maxPoint;
};

RWStructuredBuffer<ClusterBounds> out_Bounds;
ConstantBuffer<FrameUniforms> frame;

float4 Unproject(float4 coord) {
    float4 res = mul(frame.invViewProj, coord);
    return res / res.w;
}

[numthreads(CLUSTER_GRID_WIDTH, CLUSTER_GRID_HEIGHT, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= CLUSTER_GRID_WIDTH || tid.y >= CLUSTER_GRID_HEIGHT || tid.z >= CLUSTER_GRID_DEPTH)
        return;
    uint cIdx = tid.x + (tid.y * CLUSTER_GRID_WIDTH) + (tid.z * CLUSTER_GRID_WIDTH * CLUSTER_GRID_HEIGHT);

    float2 ts = float2(2.0f / float(CLUSTER_GRID_WIDTH), 2.0f / float(CLUSTER_GRID_HEIGHT));

    // UN-FLIPPED: NDC Y coordinates now map natively to Vulkan Y-down
    // tid.y = 0 maps to the top and CLUSTER_GRID_HEIGHT maps to the bottom.
    float4 ndc[4] = { float4(-1.0f + tid.x * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f), float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + tid.y * ts.y, 0.0f, 1.0f),
                      float4(-1.0f + (tid.x + 1) * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f),
                      float4(-1.0f + tid.x * ts.x, -1.0f + (tid.y + 1) * ts.y, 0.0f, 1.0f) };

    float3 pNear[4];
    float3 pFar[4];
    for (int i = 0; i < 4; ++i) {
        pNear[i] = Unproject(float4(ndc[i].xy, 0.0f, 1.0f)).xyz;
        pFar[i]  = Unproject(float4(ndc[i].xy, 1.0f, 1.0f)).xyz;
    }

    float n     = 0.1f;
    float f     = 1000.0f;
    float sNear = n * pow(f / n, float(tid.z) / float(CLUSTER_GRID_DEPTH));
    float sFar  = n * pow(f / n, float(tid.z + 1) / float(CLUSTER_GRID_DEPTH));

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
