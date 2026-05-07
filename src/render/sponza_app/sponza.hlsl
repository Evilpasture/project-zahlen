#pragma pack_matrix(column_major)

struct PushConstants {
    float4x4 mvp;
    float4x4 lightSpaceMatrix;
    float4 camPos;
    float4 lightDir;   // Ensure this matches your C++ struct
    uint albedoIdx;
    uint normalIdx;
    uint pbrIdx;
    uint _pad;
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

// --- Helper Functions ---

float3 ToLinear(float3 srgb) {
    return pow(srgb, 2.2);
}

float CalculateShadow(float4 shadowPos, float3 worldNormal, float3 lightDir) {
    float3 projCoords = shadowPos.xyz / shadowPos.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y;

    // --- MAGIC FIX FOR PETER PANNING ---
    // Calculate a bias that scales with the slope of the surface
    float bias = max(0.005 * (1.0 - dot(worldNormal, lightDir)), 0.0005);
    
    // Offset the position slightly along the normal to prevent acne 
    // without detaching the shadow (Peter Panning)
    projCoords.z -= bias;

    if (projCoords.z > 1.0) return 1.0;

    return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z).r;
}

// ACES Filmic Tonemapping
// Standard curve used in Unreal Engine and film to map HDR to LDR
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
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

PSInput VSMain([[vk::location(0)]] float3 pos : POSITION, 
               [[vk::location(1)]] float3 normal : NORMAL, 
               [[vk::location(2)]] float2 uv : TEXCOORD) {
    PSInput output;
    output.pos = mul(pc.mvp, float4(pos, 1.0));
    output.worldPos = pos; 
    output.normal = normal;
    output.uv = uv;
    output.shadowPos = mul(pc.lightSpaceMatrix, float4(pos, 1.0));
    return output;
}

float4 PSMain(PSInput input) : SV_TARGET {
    // 1. Sample Bindless
    float4 albedoSample = globalTextures[pc.albedoIdx].Sample(defaultSampler, input.uv);
    if (albedoSample.a < 0.5) discard; 
    float3 albedo = albedoSample.rgb; 
    
    float3 normalSample = globalTextures[pc.normalIdx].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
    float4 pbrSample = globalTextures[pc.pbrIdx].Sample(defaultSampler, input.uv);
    
    // Intel Sponza GLTF Packing: R = AO, G = Roughness, B = Metallic
    float ao = pbrSample.r;
    float roughness = pbrSample.g;
    float metallic = pbrSample.b;

    // 2. TBN Normal Mapping
    float3 Q1 = ddx(input.worldPos);
    float3 Q2 = ddy(input.worldPos);
    float2 st1 = ddx(input.uv);
    float2 st2 = ddy(input.uv);
    float3 N = normalize(input.normal);
    float3 T = normalize(Q1 * st2.y - Q2 * st1.y);
    float3 B = -normalize(cross(N, T));
    float3x3 TBN = float3x3(T, B, N);
    float3 worldNormal = normalize(mul(normalSample, TBN));

    // 3. Vectors
    float3 V = normalize(pc.camPos.xyz - input.worldPos);
    float3 L = normalize(-pc.lightDir.xyz); 
    float3 H = normalize(V + L);
    float NdotV = max(dot(worldNormal, V), 0.0001);
    float NdotL = max(dot(worldNormal, L), 0.0001);

    // 4. PBR MATH
    float3 F0 = float3(0.04, 0.04, 0.04); 
    F0 = lerp(F0, albedo, metallic);

    float D = DistributionGGX(worldNormal, H, roughness);
    float G = GeometrySmith(worldNormal, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    float3 specular = (D * G * F) / (4.0 * NdotV * NdotL + 0.0001);
    float3 kS = F;
    float3 kD = (float3(1.0, 1.0, 1.0) - kS) * (1.0 - metallic);
    
    // 5. SHADOWS
    float shadow = CalculateShadow(input.shadowPos, worldNormal, L);

    // 6. LIGHTING COMPOSITION
    // Direct Light (Sun) - High intensity for HDR
    float3 sunColor = float3(1.0, 0.98, 0.9);
    float3 directLight = (kD * albedo / 3.14159 + specular) * sunColor * 12.0 * NdotL * shadow;

    // 7. ENHANCED AMBIENT
    float up = worldNormal.y * 0.5 + 0.5;
    float3 skyColor = float3(0.5, 0.7, 1.0) * 1.5;    
    float3 groundColor = float3(0.2, 0.18, 0.15) * 1.2;
    float3 ambientEnv = lerp(groundColor, skyColor, up);
    
    // --- THE AO FIX ---
    // Instead of: ambient = env * albedo * ao
    // Use a lerp to "soften" the AO. 
    // 0.5 means the shadows will never be darker than 50% of the ambient light.
    float aoStrength = 0.5; 
    float aoSafe = lerp(1.0, ao, aoStrength);
    
    float3 ambient = ambientEnv * albedo * aoSafe * 1.8;
    
    float3 color = ambient + directLight;

    // 8. FINAL TOUCHES
    color *= 0.8;            // Exposure / Global Brightness adjustment
    color = ACESFilm(color); // HDR -> LDR
    color = pow(color, 1.0/2.2); // Gamma correction

    return float4(color, 1.0);
}