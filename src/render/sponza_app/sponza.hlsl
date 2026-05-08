#pragma pack_matrix(column_major)

struct Light {
    float3 position;
    uint type; 
    float3 color;
    float intensity;
    float3 direction;
    float range;
    float innerConeCos;
    float outerConeCos;
};

struct PushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4x4 worldMatrix; // NEW: Added to transform local vertex data to world space
    float4 camPos;
    float4 lightDir;
    uint albedoIdx;
    uint normalIdx;
    uint pbrIdx;
    uint lightmapIdx;
    uint emissiveIdx;
    uint lightCount; 
    uint _pad[1];   
};

[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] SamplerState lightmapSampler;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights; // Punctual lights buffer

struct PSInput {
    float4 pos : SV_POSITION;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT; // NEW: glTF Tangents are float4 (w is bitangent sign)
    float2 uv0 : TEXCOORD0; 
    float2 uv1 : TEXCOORD1; 
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

// PBR Math Functions
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

// Punctual Light Attenuation
float GetDistanceAttenuation(float dist, float range) {
    if (range <= 0.0) return 1.0 / max(dist * dist, 0.01);
    float atten = saturate(1.0 - pow(dist / range, 4.0));
    return (atten * atten) / max(dist * dist, 0.01);
}

float GetAngleAttenuation(float3 L, float3 lightDir, float inner, float outer) {
    float cd = dot(lightDir, L);
    float atten = saturate((cd - outer) / (inner - outer));
    return atten * atten;
}

// --- Main Entry ---

PSInput VSMain(
    [[vk::location(0)]] float3 pos : POSITION, 
    [[vk::location(1)]] float3 normal : NORMAL, 
    [[vk::location(2)]] float4 tangent : TANGENT, // Updated to float4
    [[vk::location(3)]] float2 uv0 : TEXCOORD0,
    [[vk::location(4)]] float2 uv1 : TEXCOORD1
) {
    PSInput output;
    // 1. Transform to Clip Space for the GPU
    output.pos = mul(pc.mvp, float4(pos, 1.0));

    // 2. Transform to World Space for Lighting (The fix)
    output.worldPos = mul(pc.worldMatrix, float4(pos, 1.0)).xyz;
    
    // 3. Transform Vectors to World Space (using 3x3 part of world matrix)
    float3x3 world3x3 = (float3x3)pc.worldMatrix;
    output.normal = normalize(mul(world3x3, normal));
    output.tangent.xyz = normalize(mul(world3x3, tangent.xyz));
    output.tangent.w = tangent.w; // Carry the sign bit for bitangent calculation

    output.uv0 = uv0;
    output.uv1 = uv1;
    
    // 4. Transform to Light Space for Shadows
    output.shadowPos = mul(pc.lightSpaceMatrix, float4(pos, 1.0));
    
    return output;
}


float4 PSMain(PSInput input) : SV_TARGET {
    // 1. Samples
    float4 albedoSample = globalTextures[pc.albedoIdx].Sample(defaultSampler, input.uv0);
    if (albedoSample.a < 0.5) discard; 
    float3 albedo = albedoSample.rgb;
    
    // Normal map is in [0, 1], move to [-1, 1]
    float3 normalMap = globalTextures[pc.normalIdx].Sample(defaultSampler, input.uv0).rgb * 2.0 - 1.0;
    
    float4 pbrSample = globalTextures[pc.pbrIdx].Sample(defaultSampler, input.uv0);
    float3 emissive = globalTextures[pc.emissiveIdx].Sample(defaultSampler, input.uv0).rgb;
    float3 lightmap = globalTextures[pc.lightmapIdx].Sample(lightmapSampler, input.uv1).rgb;

    float roughness = pbrSample.g;
    float metallic = pbrSample.b;

    // 2. High-Quality Normal Mapping using Vertex Tangents
    float3 N = normalize(input.normal);
    float3 T = normalize(input.tangent.xyz);
    // Bitangent is orthogonal to N and T. w component determines handedness.
    float3 B = cross(N, T) * input.tangent.w; 
    float3x3 TBN = float3x3(T, B, N);
    
    // Transform normal map from Tangent Space to World Space
    float3 worldNormal = normalize(mul(normalMap, TBN));

    // 3. Shared PBR Prep
    float3 V = normalize(pc.camPos.xyz - input.worldPos);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // 4. MAIN SUN CALCULATION (Directional + Shadows)
    float3 L_sun = normalize(-pc.lightDir.xyz);
    float3 H_sun = normalize(V + L_sun);
    float NdotL_sun = max(dot(worldNormal, L_sun), 0.0);
    float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);
    
    float D_s = DistributionGGX(worldNormal, H_sun, roughness);
    float G_s = GeometrySmith(worldNormal, V, L_sun, roughness);
    float3 F_s = FresnelSchlick(max(dot(H_sun, V), 0.0), F0);
    float3 specular_sun = (D_s * G_s * F_s) / (4.0 * NdotV * NdotL_sun + 0.0001);
    float3 kD_sun = (float3(1.0, 1.0, 1.0) - F_s) * (1.0 - metallic);
    
    float3 totalDirect = (kD_sun * albedo / 3.14159 + specular_sun) * float3(1.0, 0.98, 0.9) * 10.0 * NdotL_sun * shadow;

    // 5. PUNCTUAL LIGHTS LOOP
    for (uint i = 0; i < pc.lightCount; i++) {
        Light light = lights[i];
        float3 L_p;
        float atten = 1.0;

        if (light.type == 0) { // Directional
            L_p = normalize(-light.direction);
        } else { // Point / Spot
            float3 dir = light.position - input.worldPos;
            float d = length(dir);
            L_p = normalize(dir);
            atten = GetDistanceAttenuation(d, light.range);
            if (light.type == 2) { // Spot
                atten *= GetAngleAttenuation(L_p, -light.direction, light.innerConeCos, light.outerConeCos);
            }
        }

        float NdotL_p = max(dot(worldNormal, L_p), 0.0);
        if (NdotL_p > 0.0) {
            float3 H_p = normalize(V + L_p);
            float D_p = DistributionGGX(worldNormal, H_p, roughness);
            float G_p = GeometrySmith(worldNormal, V, L_p, roughness);
            float3 F_p = FresnelSchlick(max(dot(H_p, V), 0.0), F0);

            float3 spec_p = (D_p * G_p * F_p) / (4.0 * NdotV * NdotL_p + 0.0001);
            float3 kD_p = (float3(1.0, 1.0, 1.0) - F_p) * (1.0 - metallic);
            
            totalDirect += (kD_p * albedo / 3.14159 + spec_p) * light.color * light.intensity * atten * NdotL_p;
        }
    }

    // 6. FINAL COMPOSITION
    float3 ambient = lightmap * albedo * lerp(1.0, pbrSample.r, 0.5);
    float3 finalColor = ambient + totalDirect + (emissive * 4.0);

    // IMPORTANT: DO NOT apply ACESFilm or Gamma here! 
    // We are outputting Linear HDR to the R16G16B16A16 target.
    // The FXAA pass will handle the tonemapping.
    return float4(finalColor, 1.0);
}