// resources/shaders/decal.hlsl
#pragma pack_matrix(column_major) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"

struct DecalPushConstants {
    float4x4 worldMatrix;
    float4x4 invWorldMatrix;
    uint     albedoIndex;
    uint     normalIndex;
    float    roughness;
    float    metallic;
};
// SLANG WARNING: This push constant block size (~144 bytes) exceeds Vulkan's guaranteed minimum maxPushConstantsSize (128).
// DXC silently allows it, but Slang's SPIR-V legalization validates against VkPhysicalDeviceLimits::maxPushConstantsSize.
// Desktop GPUs typically support 256, but mobile/portable targets may fail. Consider splitting into ConstantBuffer for portability or verify device limit.
// Modern Slang idiom: use ParameterBlock<FrameUniforms> or ConstantBuffer for large per-frame data, keep push constants <128 bytes.
[[vk::push_constant]] DecalPushConstants pc;

struct VSOutput {
    float4 pos : SV_Position;
};

// Procedural unit cube corners ranging from [-0.5, 0.5]
static const float3 CubeVertices[8] = {float3(-0.5, -0.5, -0.5), float3(0.5, -0.5, -0.5), float3(0.5, 0.5, -0.5), float3(-0.5, 0.5, -0.5),
                                       float3(-0.5, -0.5, 0.5),  float3(0.5, -0.5, 0.5),  float3(0.5, 0.5, 0.5),  float3(-0.5, 0.5, 0.5)};

static const uint CubeIndices[36] = {
    0, 2, 1, 0, 3, 2, // Front
    4, 5, 6, 4, 6, 7, // Back
    0, 1, 5, 0, 5, 4, // Bottom
    2, 3, 7, 2, 7, 6, // Top
    0, 4, 7, 0, 7, 3, // Left
    1, 2, 6, 1, 6, 5  // Right
};

// --- Set 1: Global Bindless Layout (Inherited from Engine) ---
[[vk::binding(0, 1)]] Texture2D                     globalTextures[];
[[vk::binding(1, 1)]] SamplerState                  defaultSampler;
[[vk::binding(2, 1)]] ConstantBuffer<FrameUniforms> frame;

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    float3   localPos = CubeVertices[CubeIndices[vertexID]];
    float4   worldPos = mul(pc.worldMatrix, float4(localPos, 1.0f));
    output.pos        = mul(frame.viewProj, worldPos);
    return output;
}

// --- Set 0: Custom Decal Layout ---
[[vk::binding(0, 0)]] Texture2D<float> texDepth;
[[vk::binding(1, 0)]] SamplerState     pointSampler;

struct PSOutput {
    float4 albedo : SV_Target0;          // Writes to G-Buffer SceneColor (Slot 0)
    float4 normalRoughness : SV_Target1; // Writes to G-Buffer NormRough (Slot 1)
};

PSOutput PSMain(VSOutput input) {
    PSOutput output;

    // 1. Reconstruct screen-space UV coordinates
    float2 uv = input.pos.xy / frame.screenResolution;

    // 2. Sample Depth
    float depth = texDepth.SampleLevel(pointSampler, uv, 0).r;
    if (depth >= 1.0f) {
        discard;
    }

    // 3. Reconstruct World Position of G-Buffer geometry
    float3 worldPos = ReconstructWorldPos(uv, depth, frame.invViewProj);

    // 4. Transform World Position to Decal's Local Space
    float4 localPos = mul(pc.invWorldMatrix, float4(worldPos, 1.0f));

    // 5. Clip pixel if it lies outside the projection cube bounds [-0.5, 0.5]
    clip(0.5f - abs(localPos.xyz));

    // 6. Project UV: Map local XY [-0.5, 0.5] to texture UV [0.0, 1.0]
    float2 decalUV = localPos.xy + 0.5f;

    // 7. Sample Decal Albedo
    float4 albedo = globalTextures[pc.albedoIndex].Sample(defaultSampler, decalUV);
    if (albedo.a < 0.01f) {
        discard;
    }

    output.albedo = albedo;

    // 8. Normal Mapping
    float3 N         = float3(0.0f, 0.0f, 1.0f);
    float  roughness = pc.roughness;
    float  metallic  = pc.metallic;

    if (pc.normalIndex != 2) { // 2 is flat normal fallback
        float3 normalMap = globalTextures[pc.normalIndex].Sample(defaultSampler, decalUV).rgb * 2.0f - 1.0f;

        // Aligns local tangent space to world axes
        float3 T      = normalize(mul((float3x3) pc.worldMatrix, float3(1, 0, 0)));
        float3 B      = normalize(mul((float3x3) pc.worldMatrix, float3(0, 1, 0)));
        float3 worldN = normalize(mul((float3x3) pc.worldMatrix, float3(0, 0, 1)));

        N = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * worldN);
    } else {
        // Fallback: orient parallel to world face normal
        N = normalize(mul((float3x3) pc.worldMatrix, float3(0, 0, 1)));
    }

    float2 packedNormal    = PackNormalOctahedron(N) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    return output;
}
