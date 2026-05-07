#pragma pack_matrix(column_major)

struct PushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4 camPos;
    uint albedoIdx;
    uint normalIdx;
};

[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;

struct PSInput {
    float4 pos : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 shadowPos : TEXCOORD1; 
};

// ACES Filmic Tonemapping
float3 ACESFilm(float3 x) {
    float a = 2.51f; float b = 0.03f; float c = 2.43f; float d = 0.59f; float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

PSInput VSMain([[vk::location(0)]] float3 pos : POSITION, [[vk::location(1)]] float3 normal : NORMAL, [[vk::location(2)]] float2 uv : TEXCOORD) {
    PSInput output;
    output.pos = mul(pc.mvp, float4(pos, 1.0));
    output.worldPos = pos; 
    output.normal = normal;
    output.uv = uv;
    output.shadowPos = mul(pc.lightSpaceMatrix, float4(pos, 1.0));
    return output;
}

float4 PSMain(PSInput input) : SV_Target {
    float4 albedoSample = globalTextures[pc.albedoIdx].Sample(defaultSampler, input.uv);
    if (albedoSample.a < 0.5) discard;

    // 1. Normal Mapping
    float3 N = normalize(input.normal);
    float3 mapN = globalTextures[pc.normalIdx].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
    N = normalize(N + mapN * 0.5); // Simple detail injection

    // 2. Shadows
    float3 proj = input.shadowPos.xyz / input.shadowPos.w;
    float2 uv = proj.xy * 0.5 + 0.5;
    float shadow = 1.0;
    if (all(uv >= 0.0) && all(uv <= 1.0) && proj.z <= 1.0) {
        shadow = shadowMap.SampleCmpLevelZero(shadowSampler, uv, proj.z - 0.001).r;
    }

    // 3. Lighting
    float3 L = normalize(float3(40.0, 60.0, 40.0)); // Match lightView Eye
    float3 V = normalize(pc.camPos.xyz - input.worldPos);
    float3 H = normalize(L + V);

    float diff = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);

    float3 lighting = albedoSample.rgb * (0.05 + shadow * (diff + spec * 0.15));

    // 4. Final Polish
    float3 color = ACESFilm(lighting);
    return float4(pow(color, 1.0 / 2.2), 1.0);
}