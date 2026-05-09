#pragma pack_matrix(column_major)

// --- Constant Buffer (Push Constants) ---
struct PBRPushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4x4 worldMatrix;
    float4 camPos;
    float4 lightDir;
    uint albedoIdx;
    uint normalIdx;
    uint pbrIdx;
    uint lightmapIdx;
    uint emissiveIdx;
    uint lightCount;
};
[[vk::push_constant]] PBRPushConstants pc;

// --- Bindings ---
[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;

struct VSInput {
    float3 pos : POSITION;
    float3 norm : NORMAL;
    float4 tangent : TANGENT;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

struct PSInput {
    float4 pos : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float3 norm : NORMAL;
    float4 shadowPos : TEXCOORD1;
};

// ============================================================================
// VERTEX SHADERS
// ============================================================================

PSInput VSMain(VSInput input) {
    PSInput output;
    output.pos = mul(pc.mvp, float4(input.pos, 1.0));
    output.uv0 = input.uv0;
    output.norm = mul((float3x3)pc.worldMatrix, input.norm);
    output.shadowPos = mul(pc.lightSpaceMatrix, float4(input.pos, 1.0));
    return output;
}

// Shadow-only Vertex Shader
float4 VSShadow(VSInput input) : SV_POSITION {
    return mul(pc.mvp, float4(input.pos, 1.0));
}

// ============================================================================
// PIXEL SHADERS
// ============================================================================

float4 PSMain(PSInput input) : SV_Target {
    float3 albedo = globalTextures[pc.albedoIdx].Sample(defaultSampler, input.uv0).rgb;
    float3 norm = normalize(input.norm);
    
    // Simple Shadow Mapping (PCF)
    float shadow = shadowMap.SampleCmpLevelZero(shadowSampler, input.shadowPos.xy, input.shadowPos.z).r;
    
    // Simple Lambertian lighting with the sun
    float NdotL = saturate(dot(norm, -pc.lightDir.xyz));
    float3 color = albedo * (NdotL * shadow + 0.1); // Add ambient
    
    return float4(color, 1.0);
}

// Depth-only Pixel Shader for shadows
void PSShadow() {}