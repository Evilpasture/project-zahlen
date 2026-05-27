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
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint3 padding; // Explicit 12-byte alignment padding
	float4 baseColorFactor;
};

struct InstanceData {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint vertexCount;
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint2 padding; // Explicit 8-byte alignment padding
	float4 baseColorFactor;
};

[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
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
	float4 color : COLOR;
	nointerpolation uint4 materialIndices : TEXCOORD4;
	nointerpolation float4 baseColorFactor : TEXCOORD5;
	nointerpolation float3 pbrFactors : TEXCOORD6; // x=metallic, y=roughness, z=alphaCutoff
	nointerpolation uint alphaMode : TEXCOORD7;
};

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID) {
	VSOutput output;

	float4x4 worldMatrix;
	float4x4 prevWorldMatrix;
	uint albedoIdx, normalIdx, pbrIdx, emissiveIdx;
	float4 baseColorFactor;
	float metallicFactor, roughnessFactor, alphaCutoff;
	uint alphaMode;

	// --- FIX: Correct CPU/GPU Path Routing ---
	if (obj.isShadowPass != 0 || obj.albedoIdx != 0) {
		// --- CPU TRADITIONAL PATH ---
		worldMatrix = obj.world;
		prevWorldMatrix = obj.prevWorld;
		albedoIdx = obj.albedoIdx;
		normalIdx = obj.normalIdx;
		pbrIdx = obj.pbrIdx;
		emissiveIdx = obj.emissiveIdx;
		baseColorFactor = obj.baseColorFactor;
		metallicFactor = obj.metallicFactor;
		roughnessFactor = obj.roughnessFactor;
		alphaCutoff = obj.alphaCutoff;
		alphaMode = obj.alphaMode;
	} else {
		// --- GPU CULLING PATH ---
		InstanceData inst = g_instances[instanceId];
		worldMatrix = inst.world;
		prevWorldMatrix = inst.prevWorld;
		albedoIdx = inst.albedoIdx;
		normalIdx = inst.normalIdx;
		pbrIdx = inst.pbrIdx;
		emissiveIdx = inst.emissiveIdx;
		baseColorFactor = inst.baseColorFactor;
		metallicFactor = inst.metallicFactor;
		roughnessFactor = inst.roughnessFactor;
		alphaCutoff = inst.alphaCutoff;
		alphaMode = inst.alphaMode;
	}

	float4 worldPos = mul(worldMatrix, float4(input.position, 1.0f));
	output.worldPos = worldPos.xyz;

	if (obj.isShadowPass != 0) {
		output.pos = worldPos;
		output.uv = input.uv;
		output.baseColorFactor = baseColorFactor;
		output.pbrFactors = float3(metallicFactor, roughnessFactor, alphaCutoff);
		output.alphaMode = alphaMode;
		output.materialIndices = uint4(albedoIdx, 0, 0, 0);

		output.currClip = 0;
		output.prevClip = 0;
		output.normal = 0;
		output.tangent = 0;
		output.shadowPos = 0;
		output.color = input.color;
		return output;
	}

	output.currClip = mul(frame.viewProj, worldPos);
	output.pos = output.currClip;

	float4 prevWorldPos = mul(prevWorldMatrix, float4(input.position, 1.0f));
	output.prevClip = mul(frame.prevViewProj, prevWorldPos);

	float3x3 world3x3 = (float3x3)worldMatrix;

	// --- NEW: UNPACK NORMALS AND TANGENTS FROM UNORM [0, 1] TO [-1, 1] ---
	float3 unpackedNormal = input.normal * 2.0f - 1.0f;
	output.normal = normalize(mul(world3x3, unpackedNormal));

	float3 unpackedTangent = input.tangent.xyz * 2.0f - 1.0f;
	output.tangent.xyz = normalize(mul(world3x3, unpackedTangent));
	output.tangent.w = input.tangent.w; // W holds the bitangent sign, no unpack needed

	output.uv = input.uv;
	output.shadowPos = mul(frame.lightSpaceMatrix, worldPos);
	output.color = input.color;
	output.materialIndices = uint4(albedoIdx, normalIdx, pbrIdx, emissiveIdx);
	output.baseColorFactor = baseColorFactor;
	output.pbrFactors = float3(metallicFactor, roughnessFactor, alphaCutoff);
	output.alphaMode = alphaMode;

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

// --- SPECIALIZED SHADOW PASS ENTRY POINT ---
// Since we only evaluate the Alpha Mask discard and return void,
// this will NEVER trigger a VkCmdDraw() unconsumed target warning!
void PSShadow(VSOutput input) {
	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1) { // MASK
		if (albedo.a < alphaCutoff) {
			discard;
		}
	}
}

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	// Sample Bindless Textures
	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float metallicFactor = input.pbrFactors.x;
	float roughnessFactor = input.pbrFactors.y;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;

	// glTF PBR Math: Albedo Sample * Base Color Factor * Vertex Color
	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	// Alpha Masking
	if (alphaMode == 1) { // MASK
		if (albedo.a < alphaCutoff) {
			discard;
		}
	}

	float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
	float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);
	float3 emissive = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;

	// glTF PBR Math: PBR Sample * Parameter Factors
	float roughness = pbr.g * roughnessFactor;
	float metallic = pbr.b * metallicFactor;

	// --- FIX: Safe Tangent Fallback (Prevents NaN division-by-zero) ---
	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	if (any(input.tangent.xyz)) {
		float3 T = normalize(input.tangent.xyz);
		float3 B = normalize(cross(N, T) * input.tangent.w);
		worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
	}

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

	float3 ambient = albedo.rgb * 0.05; // Basic ambient fallback

	// Preserve exact base color alpha for BLEND transparency pipelines
	output.color = float4(ambient + directSun + directPunctual + emissive, albedo.a);

	// Calculate Motion Vectors for TAA
	float2 ndcCurr = input.currClip.xy / input.currClip.w;
	float2 ndcPrev = input.prevClip.xy / input.prevClip.w;
	output.velocity = (ndcCurr - ndcPrev) * 0.5f;

	return output;
}
