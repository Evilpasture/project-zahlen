// resources/shaders/toon.hlsl
#include "common.hlsl"

VSOutput VSMain(VSInput input, uint instanceId : SV_InstanceID) {
	VSOutput output;

	float4x4 worldMatrix;
	float4x4 prevWorldMatrix;
	uint albedoIdx, normalIdx, pbrIdx, emissiveIdx;
	float4 baseColorFactor;
	float metallicFactor, roughnessFactor, alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;

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
		jointOffset = obj.jointOffset;
		isSkinned = obj.isSkinned;
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
		jointOffset = inst.jointOffset;
		isSkinned = inst.isSkinned;
	}

	float4 localPos = float4(input.position, 1.0f);
	float3 localNormal = input.normal * 2.0f - 1.0f;
	float3 localTangent = input.tangent.xyz * 2.0f - 1.0f;

	if (isSkinned != 0) {
		localPos = SkinPosition(localPos, input.joints, input.weights, jointOffset);
		localNormal =
			normalize(SkinDirection(localNormal, input.joints, input.weights, jointOffset));
		localTangent =
			normalize(SkinDirection(localTangent, input.joints, input.weights, jointOffset));
	}

	float4 worldPos = mul(worldMatrix, localPos);
	output.worldPos = worldPos.xyz;

	output.pos = mul(frame.viewProj, worldPos);
	output.currClip = mul(frame.unjitteredViewProj, worldPos);

	float4 prevWorldPos;
	if (isSkinned != 0) {
		// --- FIX: Use SkinPositionPrev instead of SkinPosition ---
		prevWorldPos = mul(prevWorldMatrix,
						   SkinPositionPrev(localPos, input.joints, input.weights, jointOffset));
	} else {
		prevWorldPos = mul(prevWorldMatrix, localPos);
	}
	output.prevClip = mul(frame.prevUnjitteredViewProj, prevWorldPos);
	float3x3 world3x3 = (float3x3)worldMatrix;

	output.normal = normalize(mul(world3x3, localNormal));
	output.tangent.xyz = normalize(mul(world3x3, localTangent));
	output.tangent.w = input.tangent.w;

	output.uv = input.uv;
	output.shadowPos = mul(frame.lightSpaceMatrix, worldPos);
	output.color = input.color;
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

	if (any(input.tangent.xyz)) {
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

	// FIX: TAA register overflow safeguard
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
