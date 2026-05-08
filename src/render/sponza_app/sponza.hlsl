#pragma pack_matrix(column_major)

struct PushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4 camPos;
    float4 lightDir;
    uint albedoIdx;
    uint normalIdx;
    uint pbrIdx;
    uint lightmapIdx; // Successfully mapped from C++
};

[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] SamplerState lightmapSampler;

struct PSInput {
    float4 pos : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float2 uv0 : TEXCOORD0; // For Albedo/Normal/PBR
    float2 uv1 : TEXCOORD1; // For Prebaked Lightmaps
    float4 shadowPos : TEXCOORD2; 
};

// --- Helper Functions ---

float3 ToLinear(float3 srgb) {
    return pow(srgb, 2.2);
}

float CalculateShadow(float4 shadowPos, float3 worldNormal, float3 lightDir) {
    float3 projCoords = shadowPos.xyz / shadowPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y;

    float bias = max(0.005 * (1.0 - dot(worldNormal, lightDir)), 0.0005);
    projCoords.z -= bias;

    if (projCoords.z > 1.0) return 1.0;
    return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z).r;
}

float3 ACESFilm(float3 x) {
    float a = 2.51f; float b = 0.03f; float c = 2.43f; float d = 0.59f; float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// --- PBR Math ---

float DistributionGGX(float3 N, float3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return nom / (3.14159 * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) * 
           GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// --- Entry Points ---

PSInput VSMain(
    [[vk::location(0)]] float3 pos : POSITION, 
    [[vk::location(1)]] float3 normal : NORMAL, 
    [[vk::location(2)]] float2 uv0 : TEXCOORD0,
    [[vk::location(3)]] float2 uv1 : TEXCOORD1 // Added location 3 for lightmap UVs
) {
    PSInput output;
    output.pos = mul(pc.mvp, float4(pos, 1.0));
    output.worldPos = pos; 
    output.normal = normal;
    output.uv0 = uv0;
    output.uv1 = uv1;
    output.shadowPos = mul(pc.lightSpaceMatrix, float4(pos, 1.0));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // 1. Sample Bindless Material Textures (UV0)
    float4 albedoSample = globalTextures[pc.albedoIdx].Sample(defaultSampler, input.uv0);
    if (albedoSample.a < 0.5) discard; 
    float3 albedo = albedoSample.rgb; // Hardware _SRGB format handles linearization
    
    float3 normalSample = globalTextures[pc.normalIdx].Sample(defaultSampler, input.uv0).rgb * 2.0 - 1.0;
    float4 pbrSample = globalTextures[pc.pbrIdx].Sample(defaultSampler, input.uv0);
    
    // Intel Sponza GLTF Packing: R = AO, G = Roughness, B = Metallic
    float ao = pbrSample.r;
    float roughness = pbrSample.g;
    float metallic = pbrSample.b;

    // 2. Sample Prebaked Lightmap (UV1)
    // This provides high-quality indirect bounce light (Global Illumination)
    float3 lightmap = globalTextures[pc.lightmapIdx].Sample(lightmapSampler, input.uv1).rgb;

    // 3. TBN Normal Mapping
    float3 Q1 = ddx(input.worldPos);
    float3 Q2 = ddy(input.worldPos);
    float2 st1 = ddx(input.uv0);
    float2 st2 = ddy(input.uv0);
    float3 N = normalize(input.normal);
    float3 T = normalize(Q1 * st2.y - Q2 * st1.y);
    float3 B = -normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(normalSample, TBN));

    // 4. Vectors
    float3 V = normalize(pc.camPos.xyz - input.worldPos);
    float3 L = normalize(-pc.lightDir.xyz); 
    float3 H = normalize(V + L);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float NdotL = max(dot(worldNormal, L), 0.0001);

    // 5. PBR MATH
    float3 F0 = float3(0.04, 0.04, 0.04); 
    F0 = lerp(F0, albedo, metallic);

    float D = DistributionGGX(worldNormal, H, roughness);
    float G = GeometrySmith(worldNormal, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    float3 kS = F;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    
    // 6. SHADOWS (Direct Sun only)
    float shadow = CalculateShadow(input.shadowPos, worldNormal, L);

    // 7. LIGHTING COMPOSITION
    // Direct Light (Sun)
    float3 sunColor = float3(1.0, 0.98, 0.9);
    float3 directLight = (kD * albedo / 3.14159 + specular) * sunColor * 10.0 * NdotL * shadow;

    // 8. INDIRECT LIGHT (Using the Lightmap)
    // The lightmap already contains the "Sky" and "Bounce" colors.
    // We soften the AO here as well.
    float aoSafe = lerp(1.0, ao, 0.5);
    float3 ambient = lightmap * albedo * aoSafe;
    
    float3 color = ambient + directLight;

    // 9. FINAL TOUCHES
    color *= 1.0; 
    color = ACESFilm(color); 
    color = pow(color, 1.0/2.2); 

    return float4(color, 1.0);
}