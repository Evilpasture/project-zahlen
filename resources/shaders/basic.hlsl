// basic.hlsl
#pragma pack_matrix(column_major)
struct Light {
	float3 position;
	uint type; // 0=Dir, 1=Point, 2=Spot
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float innerConeCos;
	float outerConeCos;
	float2 padding;
};

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 prevViewProj;
	float4x4 lightSpaceMatrix;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float3 padding;
};

struct ObjectConstants {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint isShadowPass;
};

[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

struct VSInput {
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(1)]] float3 normal : NORMAL;
	[[vk::location(2)]] float4 tangent : TANGENT;
	[[vk::location(3)]] float2 uv : TEXCOORD;
	[[vk::location(4)]] float4 color : COLOR;
};

struct VSOutput {
	float4 pos : SV_Position;
	float4 currClip : TEXCOORD0;
	float4 prevClip : TEXCOORD1;
	float3 worldPos : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD2;
	float4 shadowPos : TEXCOORD3;
};

VSOutput VSMain(VSInput input) {
	VSOutput output;

	float4 worldPos = mul(obj.world, float4(input.position, 1.0f));
	output.worldPos = worldPos.xyz;

	if (obj.isShadowPass != 0) {
		output.pos = mul(frame.lightSpaceMatrix, worldPos);
		output.currClip = 0;
		output.prevClip = 0;
		output.normal = 0;
		output.tangent = 0;
		output.uv = 0;
		output.shadowPos = 0;
		return output;
	}
	output.currClip = mul(frame.viewProj, worldPos);
	output.pos = output.currClip;

	float4 prevWorldPos = mul(obj.prevWorld, float4(input.position, 1.0f));
	output.prevClip = mul(frame.prevViewProj, prevWorldPos);

	float3x3 world3x3 = (float3x3)obj.world;
	output.normal = normalize(mul(world3x3, input.normal));
	output.tangent.xyz = normalize(mul(world3x3, input.tangent.xyz));
	output.tangent.w = input.tangent.w;

	output.uv = input.uv;
	output.shadowPos = mul(frame.lightSpaceMatrix, worldPos);

	return output;
}

// --- PBR Helper Functions ---
float DistributionGGX(float3 N, float3 H, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = max(dot(N, H), 0.0);
	return a2 / (3.14159 * pow(NdotH * NdotH * (a2 - 1.0) + 1.0, 2.0));
}

float GeometrySchlickGGX(float NdotV, float roughness) {
	float k = pow(roughness + 1.0, 2.0) / 8.0;
	return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
	return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness) *
		   GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
	return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;
	if (projCoords.z > 1.0 || projCoords.z < 0.0)
		return 1.0;
	float bias = max(0.005 * (1.0 - dot(N, L)), 0.001);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

struct PSOutput {
	float4 color : SV_Target0;
	float2 velocity : SV_Target1;
};

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	// Sample Bindless Textures
	float4 albedo = globalTextures[obj.albedoIdx].Sample(defaultSampler, input.uv);
	if (albedo.a < 0.5)
		discard;

	float3 normalMap =
		globalTextures[obj.normalIdx].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
	float4 pbr = globalTextures[obj.pbrIdx].Sample(defaultSampler, input.uv);
	float3 emissive = globalTextures[obj.emissiveIdx].Sample(defaultSampler, input.uv).rgb;

	float roughness = pbr.g;
	float metallic = pbr.b;

	// Normal Mapping
	float3 N = normalize(input.normal);
	float3 T = normalize(input.tangent.xyz);
	float3 B = normalize(cross(N, T) * input.tangent.w);
	float3 worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);

	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);

	// Direct Directional (Sun) Light
	float3 L_sun = normalize(-frame.lightDir.xyz);
	float3 H_sun = normalize(V + L_sun);
	float NdotL_sun = max(dot(worldNormal, L_sun), 0.0);
	float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);

	float D = DistributionGGX(worldNormal, H_sun, roughness);
	float G = GeometrySmith(worldNormal, V, L_sun, roughness);
	float3 F = FresnelSchlick(max(dot(H_sun, V), 0.0), F0);

	float3 spec = (D * G * F) / (4.0 * max(dot(worldNormal, V), 0.0) * NdotL_sun + 0.0001);
	float3 kD = (1.0 - F) * (1.0 - metallic);
	float3 directSun = (kD * albedo.rgb / 3.14159 + spec) * 10.0 * NdotL_sun * shadow;

	// Process punctual lights (point/spots) in the SSBO
	float3 directPunctual = float3(0, 0, 0);
	for (uint i = 0; i < frame.lightCount; i++) {
		Light light = lights[i];
		float3 L = light.position - input.worldPos;
		float dist = length(L);
		L = normalize(L);

		float NdotL = max(dot(worldNormal, L), 0.0);
		float atten = 1.0 / (dist * dist + 0.01); // Standard falloff

		if (NdotL > 0.0) {
			float3 H = normalize(V + L);
			float D_p = DistributionGGX(worldNormal, H, roughness);
			float G_p = GeometrySmith(worldNormal, V, L, roughness);
			float3 F_p = FresnelSchlick(max(dot(H, V), 0.0), F0);
			float3 spec_p =
				(D_p * G_p * F_p) / (4.0 * max(dot(worldNormal, V), 0.0) * NdotL + 0.0001);
			float3 kD_p = (1.0 - F_p) * (1.0 - metallic);

			directPunctual += (kD_p * albedo.rgb / 3.14159 + spec_p) * light.color *
							  light.intensity * atten * NdotL;
		}
	}

	float3 ambient = albedo.rgb * 0.03; // Basic ambient fallback
	output.color = float4(ambient + directSun + directPunctual + emissive, 1.0f);

	// Calculate Motion Vectors for TAA
	float2 ndcCurr = input.currClip.xy / input.currClip.w;
	float2 ndcPrev = input.prevClip.xy / input.prevClip.w;
	output.velocity = (ndcCurr - ndcPrev) * 0.5f;

	return output;
}
