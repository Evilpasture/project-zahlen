// resources/shaders/basic.hlsl
#include "common.hlsl"

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
	VSOutput output;

	uint instId = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
	InstanceData inst = g_instances[instId];

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
	float4 emissiveFactor = inst.emissiveFactor;
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
		output.pos = mul(frame.lightSpaceMatrix, worldPos); // Standard, unbiased projection
		output.uv = localUV;
		output.baseColorFactor = baseColorFactor;
		output.emissiveFactor = emissiveFactor;
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

	float4 prevWorldPos;
	if (isSkinned != 0) {
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
	output.emissiveFactor = emissiveFactor;
	output.pbrFactors = float3(metallicFactor, roughnessFactor, alphaCutoff);
	output.alphaMode = alphaMode;
	return output;
}

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

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;
	float3 emissiveMap = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;
	float3 emissive = emissiveMap * input.emissiveFactor.rgb;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);

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

	output.color = float4(albedo.rgb + emissive, albedo.a);
	float2 packedNormal = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
	output.normalRoughness = float4(packedNormal, roughness, metallic);

	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);
	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);

	return output;
}
