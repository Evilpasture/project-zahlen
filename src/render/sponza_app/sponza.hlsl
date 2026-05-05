struct VSInput {
    [[vk::location(0)]] float3 pos : POSITION;
    [[vk::location(1)]] float3 norm : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct PSInput {
    float4 pos : SV_Position; 
    float3 worldNorm : NORMAL;
    float2 uv : TEXCOORD0;
    float3 viewDir : TEXCOORD1; 
    float4 lightSpacePos : EXTRA;
};

struct PushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4 camPos; 
};

[[vk::push_constant]] PushConstants pc;

PSInput VSMain(VSInput input) {
    PSInput output;
    float4 worldPos = float4(input.pos, 1.0); // <--- FIXED THIS LINE
    output.pos = mul(pc.mvp, worldPos);
    output.lightSpacePos = mul(pc.lightSpaceMatrix, worldPos); 
    output.worldNorm = input.norm;
    output.uv = input.uv;
    output.viewDir = normalize(pc.camPos.xyz - input.pos);
    return output;
}

[[vk::binding(0, 0)]] Texture2D baseColorTex;
[[vk::binding(1, 0)]] SamplerState baseColorSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler; 

float3 ACESFilm(float3 x) {
    float a = 2.51f; float b = 0.03f; float c = 2.43f; float d = 0.59f; float e = 0.14f;
    return saturate((x*(a*x+b))/(x*(c*x+d)+e));
}

float4 PSMain(PSInput input) : SV_Target {
    float3 projCoords = input.lightSpacePos.xyz / input.lightSpacePos.w;
    float2 shadowUV = projCoords.xy * 0.5 + 0.5;
    float currentDepth = projCoords.z;

    float shadow = shadowMap.SampleCmpLevelZero(shadowSampler, shadowUV, currentDepth - 0.001).r;

    float4 albedo = baseColorTex.Sample(baseColorSampler, input.uv);
    if (albedo.a < 0.5) discard;

    float3 N = normalize(input.worldNorm);
    float3 L = normalize(float3(0.5, 1.0, 0.3));
    float3 V = normalize(input.viewDir);
    float3 H = normalize(L + V);

    float hemi = dot(N, float3(0, 1, 0)) * 0.5 + 0.5;
    float3 ambient = lerp(float3(0.3, 0.2, 0.1), float3(0.2, 0.5, 1.0), hemi) * 0.2;
    
    float ndotl = max(dot(N, L), 0.0);
    float spec = pow(max(dot(N, H), 0.0), 32.0);
    
    float3 direct = (ndotl * float3(1.0, 0.9, 0.7) * 4.0) + (spec * 2.0);
    float3 finalColor = (ambient + direct * shadow) * albedo.rgb;

    return float4(pow(ACESFilm(finalColor), 1.0/2.2), albedo.a);
}