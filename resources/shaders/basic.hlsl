// resources/shaders/basic.hlsl
#include "common.hlsl"

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
	VSOutput output;

	uint instId = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
	InstanceData inst = g_instances[instId];

	// --- NEW: True Bindless Geometry Fetch ---
	uint actualVertexId = vertexId;
	if (inst.iboAddress != 0) {
		actualVertexId = vk::RawBufferLoad<uint>(inst.iboAddress + vertexId * 4, 4);
	}
	uint64_t baseAddr = inst.vboAddress + actualVertexId * 64;

	float3 position = vk::RawBufferLoad<float3>(baseAddr + 0, 4);
	uint normal = vk::RawBufferLoad<uint>(baseAddr + 12, 4);
	uint tangent = vk::RawBufferLoad<uint>(baseAddr + 16, 4);
	uint uv = vk::RawBufferLoad<uint>(baseAddr + 20, 4);
	uint color = vk::RawBufferLoad<uint>(baseAddr + 24, 4);
	uint2 joints = vk::RawBufferLoad<uint2>(baseAddr + 28, 4);
	float4 weights = vk::RawBufferLoad<float4>(baseAddr + 36, 4);

	float4x4 worldMatrix = inst.world;
	float4x4 prevWorldMatrix = inst.prevWorld;
	uint albedoIdx = inst.albedoIdx;
	uint normalIdx = inst.normalIdx;
	uint pbrIdx = inst.pbrIdx;
	uint emissiveIdx = inst.emissiveIdx;
	float4 baseColorFactor = inst.baseColorFactor;
	float metallicFactor = inst.metallicFactor;
	float roughnessFactor = inst.roughnessFactor;
	float alphaCutoff = inst.alphaCutoff;
	uint alphaMode = inst.alphaMode;
	uint jointOffset = inst.jointOffset;
	uint isSkinned = inst.isSkinned;

	uint morphOffset = inst.morphOffset;
	uint activeMorphCount = inst.activeMorphCount;
	float4 morphWeights = inst.morphWeights;
	uint vertexCount = inst.vertexCount;

	// Process values from our safe vector loads
	float4 localPos = float4(position, 1.0f);
	float3 localNormal = UnpackNormal(normal).xyz;
	float4 localTangent = UnpackNormal(tangent);
	float2 localUV = UnpackUV(uv);
	float4 localColor = UnpackColor(color);
	uint4 localJoints = UnpackJoints(joints);

	if (activeMorphCount > 0) {
		localPos.xyz += GetMorphDisplacement(actualVertexId, vertexCount, morphOffset,
											 activeMorphCount, morphWeights);
	}

	float4 worldPos;
	float3x3 world3x3 = (float3x3)worldMatrix;

	if (isSkinned != 0) {
		worldPos = mul(worldMatrix, SkinPosition(localPos, localJoints, weights, jointOffset));
		output.normal =
			normalize(mul(world3x3, SkinDirection(localNormal, localJoints, weights, jointOffset)));
		output.tangent.xyz = normalize(
			mul(world3x3, SkinDirection(localTangent.xyz, localJoints, weights, jointOffset)));
	} else {
		worldPos = mul(worldMatrix, localPos);
		output.normal = normalize(mul(world3x3, localNormal));
		output.tangent.xyz = normalize(mul(world3x3, localTangent.xyz));
	}

	output.worldPos = worldPos.xyz;

	if (obj.isShadowPass != 0) {
		output.pos = worldPos;
		output.uv = localUV;
		output.baseColorFactor = baseColorFactor;
		output.pbrFactors = float3(metallicFactor, roughnessFactor, alphaCutoff);
		output.alphaMode = alphaMode;
		output.materialIndices = uint4(albedoIdx, 0, 0, 0);

		output.currClip = 0;
		output.prevClip = 0;
		output.normal = 0;
		output.tangent = 0;
		output.shadowPos = 0;
		output.color = localColor;
		return output;
	}

	output.pos = mul(frame.viewProj, worldPos);
	output.currClip = mul(frame.unjitteredViewProj, worldPos);

	// OPTIMIZATION: Cache skinning result for previous frame to avoid redundant calculations
	float4 prevWorldPos;
	if (isSkinned != 0) {
		// Use pre-computed previous joints from the previous frame (GPU-cached)
		prevWorldPos =
			mul(prevWorldMatrix, SkinPositionPrev(localPos, localJoints, weights, jointOffset));
	} else {
		prevWorldPos = mul(prevWorldMatrix, localPos);
	}
	output.prevClip = mul(frame.prevUnjitteredViewProj, prevWorldPos);

	output.tangent.w = localTangent.w;

	output.uv = localUV;
	output.shadowPos = mul(frame.lightSpaceMatrix, worldPos);
	output.color = localColor;
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
	float NdotH = saturate(dot(N, H));
	float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);
	return a2 / (3.14159265f * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
	float k = pow(roughness + 1.0, 2.0) / 8.0;
	return NdotV / max(NdotV * (1.0 - k) + k, 0.001); // Prevent Divide by zero
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
	return GeometrySchlickGGX(saturate(dot(N, V)), roughness) *
		   GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
	return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// --- GEOMETRIC SPECULAR ANTI-ALIASING (GSAA) ---
float AntiAliasRoughness(float roughness, float3 unnormalizedNormal) {
	float r = length(unnormalizedNormal);
	r = clamp(r, 0.0001f, 1.0f);

	float alpha = roughness * roughness;

	// FIX: Clamp the specular power to >= 0.0 to prevent negative denominators and NaN division
	float power = max(2.0f / (alpha * alpha + 0.0001f) - 2.0f, 0.0f);

	float toksvig = r / (r + (1.0f - r) * power);

	float newAlpha = sqrt(2.0f / (power * toksvig + 2.0f));
	return saturate(newAlpha);
}

// --- ANISOTROPIC GGX PBR FUNCTIONS ---
float DistributionAnisotropicGGX(float3 N, float3 H, float3 T, float3 B, float alpha_x,
								 float alpha_y) {
	float HdotT = dot(H, T);
	float HdotB = dot(H, B);
	float HdotN = saturate(dot(N, H));

	float d =
		HdotT * HdotT / (alpha_x * alpha_x) + HdotB * HdotB / (alpha_y * alpha_y) + HdotN * HdotN;
	return 1.0f / (3.14159265f * alpha_x * alpha_y * d * d);
}

float SmithG1_Anisotropic(float3 N, float3 V, float3 T, float3 B, float alpha_x, float alpha_y) {
	float NdotV = saturate(dot(N, V));
	float TdotV = dot(T, V);
	float BdotV = dot(B, V);

	float v2 =
		alpha_x * alpha_x * TdotV * TdotV + alpha_y * alpha_y * BdotV * BdotV + NdotV * NdotV;
	return 2.0f * NdotV / (NdotV + sqrt(v2));
}

float GeometryAnisotropicSmith(float3 N, float3 V, float3 L, float3 T, float3 B, float alpha_x,
							   float alpha_y) {
	return SmithG1_Anisotropic(N, V, T, B, alpha_x, alpha_y) *
		   SmithG1_Anisotropic(N, L, T, B, alpha_x, alpha_y);
}

// --- SPECIALIZED SHADOW PASS ENTRY POINT ---
void PSShadow(VSOutput input) {
	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}
}

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float metallicFactor = input.pbrFactors.x;
	float roughnessFactor = input.pbrFactors.y;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;

	// Sample raw textures
	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);
	float3 emissive = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;

	float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
	float metallic = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	if (indices.y != 2 && any(input.tangent.xyz)) {
		float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
		if (dot(T_unnorm, T_unnorm) < 0.0001f) {
			T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
		}
		float3 T = normalize(T_unnorm);
		float tangentSign = input.tangent.w * 2.0f - 1.0f;
		float3 B = normalize(cross(N, T) * tangentSign);

		float3 normalMap =
			globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0f - 1.0f;
		worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
	}

	// Output raw diffuse and emissive added together in target 0
	output.color = float4(albedo.rgb + emissive, albedo.a);

	// Output packed normals and material descriptors
	float2 packedNormal = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
	output.normalRoughness = float4(packedNormal, roughness, metallic);

	// Velocity output
	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);
	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);

	return output;
}
