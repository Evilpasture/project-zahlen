// resources/shaders/punctual_shadows.slang

import common;
import uniforms;

struct PushConstants {
    uint lightIndex;
};
[vk::push_constant] PushConstants pc;

struct PunctualShadowVSOutput {
    float4 pos : SV_Position;
    float  linearDepth : TEXCOORD0;
};

// NOTE: SV_Vulkan* semantics are required here -- the CPU-built shadow
// indirect commands encode the instance slot in firstInstance, and Slang's
// SV_InstanceID would strip BaseInstance (yielding 0 for every sub-draw).
PunctualShadowVSOutput VSMain(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID, uint viewId : SV_ViewID) {
    PunctualShadowVSOutput output;

    InstanceData inst           = scene.g_instances[instanceId];
    uint         actualVertexId = inst.fetchIndex(vertexId);
    actualVertexId              = min(actualVertexId, max(inst.vertexCount, 1u) - 1u);

    float3 localPos = inst.fetchPosition(actualVertexId);
    float3 worldPos = mul(inst.world, float4(localPos, 1.0f)).xyz;

    // 2. Fetch Light Data
    Light  light    = scene.lights[pc.lightIndex];
    float3 lightPos = light.position;

    // 3. Build View Matrix for the specific Cubemap Face (Native Vulkan Orientations)
    float3 target = lightPos;
    float3 up     = float3(0, -1, 0);

    if (viewId == 0) {
        target += float3(1, 0, 0);
    } // +X
    else if (viewId == 1) {
        target += float3(-1, 0, 0);
    } // -X
    else if (viewId == 2) {
        target += float3(0, 1, 0);
        up = float3(0, 0, 1);
    } // +Y
    else if (viewId == 3) {
        target += float3(0, -1, 0);
        up = float3(0, 0, -1);
    } // -Y
    else if (viewId == 4) {
        target += float3(0, 0, 1);
    } // +Z
    else if (viewId == 5) {
        target += float3(0, 0, -1);
    } // -Z

    // Right-Handed LookAt
    float3 zaxis = normalize(lightPos - target);
    float3 xaxis = normalize(cross(up, zaxis));
    float3 yaxis = cross(zaxis, xaxis);

    float4x4 viewMat = float4x4(
        xaxis.x, xaxis.y, xaxis.z, -dot(xaxis, lightPos), yaxis.x, yaxis.y, yaxis.z, -dot(yaxis, lightPos), zaxis.x, zaxis.y, zaxis.z, -dot(zaxis, lightPos),
        0.0f, 0.0f, 0.0f, 1.0f
    );

    // Right-Handed Perspective Projection with Vulkan Y-down map
    float    n       = 0.1f;
    float    f       = max(light.range, 1.0f);
    float4x4 projMat = float4x4(1, 0, 0, 0, 0, -1, 0, 0, 0, 0, f / (n - f), -1, 0, 0, (n * f) / (n - f), 0);

    output.pos = mul(projMat, mul(viewMat, float4(worldPos, 1.0f)));

    // Store linear depth so we don't have to fight perspective Z-fighting on Omni-shadows
    output.linearDepth = length(worldPos - lightPos) / f;
    return output;
}

struct PunctualShadowPSOutput {
    float depth : SV_Depth;
};

PunctualShadowPSOutput PSMain(PunctualShadowVSOutput input) {
    PunctualShadowPSOutput output;
    // Write linear depth [0, 1] directly to the shadow cubemap
    output.depth = saturate(input.linearDepth);
    return output;
}
