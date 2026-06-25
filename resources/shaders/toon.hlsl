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
	output.emissiveFactor = inst.emissiveFactor;

	return output;
}

PSOutput PSMain(VSOutput input) {
	PSOutput output;

	uint4 indices = input.materialIndices;
	float4 baseColorFactor = input.baseColorFactor;
	float alphaCutoff = input.pbrFactors.z;
	uint alphaMode = input.alphaMode;
	float roughness = input.pbrFactors.y;

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	float3 emissiveMap = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;
	float3 emissive = emissiveMap * input.emissiveFactor.rgb;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

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

	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 L_sun = normalize(frame.lightDir.xyz);

	// --- 1. TOON LIGHTING RAMP ---
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

	// --- 2. AMBIENT ---
	float3 irradiance = EvaluateSH(worldNormal, frame.sh);
	float3 ambient = albedo.rgb * irradiance * 0.25f;

	// --- 3. NORMAL OFFSET BIAS & SHADOWS ---
	// Dynamically scale using the active shadow map settings
	float texelSizeWorld = frame.shadowWidth / frame.shadowResolution;
	// Add a baseline offset of 0.2 texels to prevent flat-surface acne at lower resolutions
	float normalBias = saturate(1.0 - dot(worldNormal, L_sun)) * 1.5 + 0.2;
	float3 biasedWorldPos = input.worldPos + worldNormal * (normalBias * texelSizeWorld);

	float4 shadowPos = mul(frame.lightSpaceMatrix, float4(biasedWorldPos, 1.0f));

	// Manually apply the Vulkan texture coordinate bias
	shadowPos.xy = shadowPos.xy * float2(0.5f, -0.5f) + 0.5f * shadowPos.w;

	float shadow = CalculateShadowPCSS(shadowPos, worldNormal, L_sun, input.pos.xy, shadowMap,
									   shadowSampler, defaultSampler, frame.shadowResolution);
	celIntensity *= shadow;

	// --- 4. TOON SPECULAR SHAPE ---
	float3 H_sun = normalize(V + L_sun);
	float NdotH_sun = max(dot(worldNormal, H_sun), 0.0f);
	float specular = step(0.98f, NdotH_sun) * 0.35f * shadow;

	// --- 5. SHARP CONTOUR OUTLINE ---
	float rim = 1.0f - saturate(dot(worldNormal, V));
	float rimIntensity = smoothstep(0.6f, 0.75f, rim) * 0.25f * celIntensity;

	// --- 6. COMPOSITING ---
	float3 finalColor = ambient + (albedo.rgb * celIntensity) + specular + rimIntensity + emissive;
	finalColor = min(finalColor, 100.0f);

	output.color = float4(finalColor, albedo.a);

	float metallic = input.pbrFactors.x;
	float2 packedNormal = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
	output.normalRoughness = float4(packedNormal, roughness, metallic);

	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);
	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);

	return output;
}
