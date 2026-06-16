// resources/shaders/toon.hlsl
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
	float metallicFactor = inst.metallicFactor;
	float roughnessFactor = inst.roughnessFactor;
	float alphaCutoff = inst.alphaCutoff;
	uint alphaMode = inst.alphaMode;
	uint jointOffset = inst.jointOffset;
	uint isSkinned = inst.isSkinned;

	// Process values from our safe vector loads
	float4 localPos = float4(position, 1.0f);
	float3 localNormal = UnpackNormal(normal).xyz;
	float4 localTangent = UnpackNormal(tangent);
	float2 localUV = UnpackUV(uv);
	float4 localColor = UnpackColor(color);
	uint4 localJoints = UnpackJoints(joints);

	float4 skinnedPos = localPos;

	if (isSkinned != 0) {
		skinnedPos = SkinPosition(localPos, localJoints, weights, jointOffset);
		localNormal = normalize(SkinDirection(localNormal, localJoints, weights, jointOffset));
		localTangent.xyz =
			normalize(SkinDirection(localTangent.xyz, localJoints, weights, jointOffset));
	}

	float4 worldPos = mul(worldMatrix, skinnedPos);
	output.worldPos = worldPos.xyz;

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
	float3x3 world3x3 = (float3x3)worldMatrix;

	output.normal = normalize(mul(world3x3, localNormal));
	output.tangent.xyz = normalize(mul(world3x3, localTangent.xyz));
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

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;
	float roughness = input.pbrFactors.y;
	// Restore texture sampling combined with base color and vertex color attributes!
	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	if (indices.y != 2 && any(input.tangent.xyz)) {
		// 1. Calculate unnormalized tangent
		float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;

		// 2. Check length BEFORE calling normalize() to prevent NaN
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
	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 L_sun = normalize(frame.lightDir.xyz);

	// --- TOON LIGHTING RAMP ---
	float NdotL_sun = dot(worldNormal, L_sun);
	float halfLambert = NdotL_sun * 0.5f + 0.5f;

	float celIntensity;
	if (halfLambert < 0.25f) {
		celIntensity = 0.25f; // Hard shadow band
	} else if (halfLambert < 0.65f) {
		celIntensity = 0.65f; // Midtone band
	} else {
		celIntensity = 1.0f; // Bright band
	}

	float3 irradiance = EvaluateSH(worldNormal, frame.sh);
	float3 ambient = albedo.rgb * irradiance * 0.25f;

	float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);
	celIntensity *= shadow;

	// --- TOON SPECULAR SHAPE ---
	float3 H_sun = normalize(V + L_sun);
	float NdotH_sun = max(dot(worldNormal, H_sun), 0.0f);
	float specular = step(0.98f, NdotH_sun) * 0.35f * shadow;

	// --- SHARP CONTOUR OUTLINE ---
	float rim = 1.0f - saturate(dot(worldNormal, V));
	float rimIntensity = smoothstep(0.6f, 0.75f, rim) * 0.25f * celIntensity;

	float3 finalColor = ambient + (albedo.rgb * celIntensity) + specular + rimIntensity;

	finalColor = min(finalColor, 100.0f);

	output.color = float4(finalColor, albedo.a);
	output.normalRoughness = float4(worldNormal * 0.5f + 0.5f, roughness);

	// Clamp W to a small positive number to prevent Divide-By-Zero NaN explosions
	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);

	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);

	return output;
}
