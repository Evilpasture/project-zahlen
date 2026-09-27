
import common;
import pbr_helpers;

struct DecalPushConstants {
    float4x4 worldMatrix;
    float4x4 clipToLocal;
    uint albedoIndex;
    uint normalIndex;
    float roughness;
    float metallic;
};
[vk::push_constant]
DecalPushConstants pc;

struct VSOutput {
    float4 pos : SV_Position;
};

static const float3 CubeVertices[8] = { float3(-0.5, -0.5, -0.5), float3(0.5, -0.5, -0.5), float3(0.5, 0.5, -0.5), float3(-0.5, 0.5, -0.5),
                                        float3(-0.5, -0.5, 0.5),  float3(0.5, -0.5, 0.5),  float3(0.5, 0.5, 0.5),  float3(-0.5, 0.5, 0.5) };

static const uint CubeIndices[36] = {
    0, 2, 1, 0, 3, 2,
    4, 5, 6, 4, 6, 7,
    0, 1, 5, 0, 5, 4,
    2, 3, 7, 2, 7, 6,
    0, 4, 7, 0, 7, 3,
    1, 2, 6, 1, 6, 5
};


VSOutput VSMain(uint vertexID: SV_VertexID) {
    VSOutput output;
    float3 localPos = CubeVertices[CubeIndices[vertexID]];
    float4 worldPos = mul(pc.worldMatrix, float4(localPos, 1.0f));
    output.pos      = mul(scene.frame.viewProj, worldPos);
    return output;
}

Texture2D<float> texDepth;
SamplerState pointSampler;

struct PSOutput {
    float4 albedo : SV_Target0;
    float4 normalRoughness : SV_Target1;
};

PSOutput PSMain(VSOutput input) {
    PSOutput output;

    float2 uv = input.pos.xy / scene.frame.screenResolution;

    float depth = texDepth.SampleLevel(pointSampler, uv, 0).r;
    if (depth >= 1.0f) {
        discard;
    }

    float4 clipPos = float4(uv.x * 2.0f - 1.0f, uv.y * 2.0f - 1.0f, depth, 1.0f);

    float4 localPos = mul(pc.clipToLocal, clipPos);
    localPos.xyz    = localPos.xyz / localPos.w;

    clip(0.5f - abs(localPos.xyz));

    float2 decalUV = localPos.xy + 0.5f;

    float4 albedo = scene.globalTextures[pc.albedoIndex].Sample(scene.defaultSampler, decalUV);
    if (albedo.a < 0.01f) {
        discard;
    }

    output.albedo = albedo;

    float3 N        = float3(0.0f, 0.0f, 1.0f);
    float roughness = pc.roughness;
    float metallic  = pc.metallic;

    if (pc.normalIndex != 2) {
        float3 normalMap = scene.globalTextures[pc.normalIndex].Sample(scene.defaultSampler, decalUV).rgb * 2.0f - 1.0f;

        float3 T      = normalize(mul((float3x3) pc.worldMatrix, float3(1, 0, 0)));
        float3 B      = normalize(mul((float3x3) pc.worldMatrix, float3(0, 1, 0)));
        float3 worldN = normalize(mul((float3x3) pc.worldMatrix, float3(0, 0, 1)));

        N = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * worldN);
    } else {
        N = normalize(mul((float3x3) pc.worldMatrix, float3(0, 0, 1)));
    }

    float2 packedNormal    = PackNormalOctahedron(N) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    return output;
}
