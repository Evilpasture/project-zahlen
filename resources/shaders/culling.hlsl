// resources/shaders/culling.hlsl
#define SKIP_BINDINGS
#include "common.hlsl"

struct DrawIndirectCommand {
    uint vertexCount;
    uint instanceCount;
    uint firstVertex;
    uint firstInstance;
};

struct CullingConstants {
    float4x4 viewProj;
    float2   hizScreenSize;
    uint     maxHiZMipLevel;
    uint     drawCount;
    uint     passIndex;
};

[[vk::push_constant]] CullingConstants cullConstants;

[[vk::binding(0, 0)]] StructuredBuffer<InstanceData>          g_instances;
[[vk::binding(1, 0)]] RWStructuredBuffer<DrawIndirectCommand> g_indirectCommands;
[[vk::binding(2, 0)]] Texture2D<float>                        g_hizTexture;
[[vk::binding(3, 0)]] SamplerState                            g_pointSampler;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>                g_secondPassCandidates;
[[vk::binding(5, 0)]] RWStructuredBuffer<uint>                g_secondPassCount;

bool SphereFrustumVisible(float3 center, float radius) {
    float4 r0 = float4(cullConstants.viewProj._m00, cullConstants.viewProj._m01, cullConstants.viewProj._m02, cullConstants.viewProj._m03);
    float4 r1 = float4(cullConstants.viewProj._m10, cullConstants.viewProj._m11, cullConstants.viewProj._m12, cullConstants.viewProj._m13);
    float4 r2 = float4(cullConstants.viewProj._m20, cullConstants.viewProj._m21, cullConstants.viewProj._m22, cullConstants.viewProj._m23);
    float4 r3 = float4(cullConstants.viewProj._m30, cullConstants.viewProj._m31, cullConstants.viewProj._m32, cullConstants.viewProj._m33);

    float4 planes[6];
    planes[0] = r3 + r0;
    planes[1] = r3 - r0;
    planes[2] = r3 + r1;
    planes[3] = r3 - r1;
    planes[4] = r2;
    planes[5] = r3 - r2;

    [unroll] for (uint i = 0; i < 6; ++i) {
        float len = length(planes[i].xyz);
        planes[i] /= max(len, 1e-6f);
        if (dot(float4(center, 1.0f), planes[i]) < -radius)
            return false;
    }
    return true;
}

bool ProjectAABB(float3 center, float radius, out float4 outUvBounds, out float outMinZ) {
    outUvBounds = float4(0.0f, 0.0f, 1.0f, 1.0f);
    outMinZ     = 0.0f;

    float3 minPos     = center - radius;
    float3 maxPos     = center + radius;
    float3 corners[8] = {float3(minPos.x, minPos.y, minPos.z), float3(maxPos.x, minPos.y, minPos.z), float3(minPos.x, maxPos.y, minPos.z),
                         float3(maxPos.x, maxPos.y, minPos.z), float3(minPos.x, minPos.y, maxPos.z), float3(maxPos.x, minPos.y, maxPos.z),
                         float3(minPos.x, maxPos.y, maxPos.z), float3(maxPos.x, maxPos.y, maxPos.z)};

    float  minZ  = 1.0f;
    float2 minUV = float2(1.0f, 1.0f);
    float2 maxUV = float2(0.0f, 0.0f);

    for (int i = 0; i < 8; ++i) {
        float4 clipPos = mul(cullConstants.viewProj, float4(corners[i], 1.0f));
        if (clipPos.w <= 0.001f) {
            return false;
        }

        float3 ndc = clipPos.xyz / clipPos.w;
        float2 uv  = ndc.xy * 0.5f + 0.5f;

        minUV = min(minUV, uv);
        maxUV = max(maxUV, uv);
        minZ  = min(minZ, ndc.z);
    }
    outUvBounds = saturate(float4(minUV, maxUV));
    outMinZ     = saturate(minZ);
    return true;
}

bool IsHiZVisible(float3 center, float radius) {
    float4 uvBounds;
    float  minZ;
    if (!ProjectAABB(center, radius, uvBounds, minZ))
        return true;

    float2 size         = (uvBounds.zw - uvBounds.xy) * cullConstants.hizScreenSize;
    float  maxDimension = max(size.x, size.y);
    float  mip          = clamp(floor(log2(max(maxDimension, 1.0f))), 0.0f, float(cullConstants.maxHiZMipLevel));

    float d0 = g_hizTexture.SampleLevel(g_pointSampler, uvBounds.xy, mip).r;
    float d1 = g_hizTexture.SampleLevel(g_pointSampler, uvBounds.zy, mip).r;
    float d2 = g_hizTexture.SampleLevel(g_pointSampler, uvBounds.xw, mip).r;
    float d3 = g_hizTexture.SampleLevel(g_pointSampler, uvBounds.zw, mip).r;

    return (minZ - 0.0001f) <= max(max(d0, d1), max(d2, d3));
}

[numthreads(64, 1, 1)] void CSMain(uint3 dispatchThreadID : SV_DispatchThreadID) {
    if (cullConstants.passIndex == 0) {
        // --- PASS 1: Frustum + Last Frame Hi-Z ---
        uint index = dispatchThreadID.x;
        if (index >= cullConstants.drawCount)
            return;

        InstanceData inst   = g_instances[index];
        float3       center = mul(inst.world, float4(inst.localCenter, 1.0f)).xyz;
        float        scaleX = length(float3(inst.world._m00, inst.world._m10, inst.world._m20));
        float        scaleY = length(float3(inst.world._m01, inst.world._m11, inst.world._m21));
        float        scaleZ = length(float3(inst.world._m02, inst.world._m12, inst.world._m22));
        float        radius = inst.cullRadius * max(scaleX, max(scaleY, scaleZ));

        bool frustumVisible = SphereFrustumVisible(center, radius);
        bool pass1Visible   = false;

        if (frustumVisible) {
            pass1Visible = IsHiZVisible(center, radius);
            if (!pass1Visible) {
                uint candidateSlot;
                InterlockedAdd(g_secondPassCount[0], 1, candidateSlot);
                if (candidateSlot < cullConstants.drawCount) {
                    g_secondPassCandidates[candidateSlot] = index;
                }
            }
        }

        DrawIndirectCommand cmd;
        cmd.vertexCount           = inst.indexCount > 0 ? inst.indexCount : inst.vertexCount;
        cmd.instanceCount         = pass1Visible ? 1u : 0u;
        cmd.firstVertex           = 0;
        cmd.firstInstance         = index;
        g_indirectCommands[index] = cmd;

    } else if (cullConstants.passIndex == 1) {
        // --- PASS 2: Current Frame Hi-Z Re-test ---
        uint candidateIdx    = dispatchThreadID.x;
        uint totalCandidates = g_secondPassCount[0];
        if (candidateIdx >= totalCandidates)
            return;

        uint         index  = g_secondPassCandidates[candidateIdx];
        InstanceData inst   = g_instances[index];
        float3       center = mul(inst.world, float4(inst.localCenter, 1.0f)).xyz;
        float        scaleX = length(float3(inst.world._m00, inst.world._m10, inst.world._m20));
        float        scaleY = length(float3(inst.world._m01, inst.world._m11, inst.world._m21));
        float        scaleZ = length(float3(inst.world._m02, inst.world._m12, inst.world._m22));
        float        radius = inst.cullRadius * max(scaleX, max(scaleY, scaleZ));

        bool pass2Visible = IsHiZVisible(center, radius);

        DrawIndirectCommand cmd;
        cmd.vertexCount           = inst.indexCount > 0 ? inst.indexCount : inst.vertexCount;
        cmd.instanceCount         = pass2Visible ? 1u : 0u;
        cmd.firstVertex           = 0;
        cmd.firstInstance         = index;
        g_indirectCommands[index] = cmd;
    }
}
