// resources/shaders/basic.hlsl
#include "common.hlsl"

VSOutput VSMain(VSInput input, uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
	VSOutput output;

	float4x4 worldMatrix;
	float4x4 prevWorldMatrix;
	uint albedoIdx, normalIdx, pbrIdx, emissiveIdx;
	float4 baseColorFactor;
	float metallicFactor, roughnessFactor, alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;

	uint morphOffset;
	uint activeMorphCount;
	float4 morphWeights;
	uint vertexCount;

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

		morphOffset = obj.morphOffset;
		activeMorphCount = obj.activeMorphCount;
		morphWeights = obj.morphWeights;
		vertexCount = obj.vertexCount; // Resolves: 'no member named vertexCount' [1]
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

		morphOffset = inst.morphOffset;
		activeMorphCount = inst.activeMorphCount;
		morphWeights = inst.morphWeights;
		vertexCount = inst.vertexCount;
	}

	// Declare as float4 initially:
	float4 localPos = float4(input.position, 1.0f);
	float3 localNormal = input.normal * 2.0f - 1.0f;
	float3 localTangent = input.tangent.xyz * 2.0f - 1.0f;

	// Apply morph target displacement:
	if (activeMorphCount > 0) {
		localPos.xyz += GetMorphDisplacement(vertexId, vertexCount, morphOffset, activeMorphCount,
											 morphWeights);
	}

	// SkinPosition will now match since we passed float4:
	if (isSkinned != 0) {
		localPos = SkinPosition(localPos, input.joints, input.weights, jointOffset);
		localNormal =
			normalize(SkinDirection(localNormal, input.joints, input.weights, jointOffset));
		localTangent =
			normalize(SkinDirection(localTangent, input.joints, input.weights, jointOffset));
	}

	float4 worldPos = mul(worldMatrix, localPos);
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

	float4 prevWorldPos = mul(prevWorldMatrix, localPos);
	output.prevClip = mul(frame.prevViewProj, prevWorldPos);

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

// --- SPECIALIZED SHADOW PASS ENTRY POINT ---
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
	float roughness = (indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor;
	float metallic = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

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
	float g_term = GeometrySmith(worldNormal, V, L_sun, roughness);
	float3 F = FresnelSchlick(max(dot(H_sun, V), 0.0), F0);

	float3 spec = (D * g_term * F) / (4.0 * max(dot(worldNormal, V), 0.0) * NdotL_sun + 0.0001);
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

	// --- NEW: FULL PBR INDIRECT IMAGE-BASED LIGHTING (IBL) ---
	float3 R = reflect(-V, worldNormal);
	float3 F_rough = FresnelSchlickRoughness(max(dot(worldNormal, V), 0.0), F0, roughness);
	float3 kS_rough = F_rough;
	float3 kD_rough = (1.0 - kS_rough) * (1.0 - metallic);

	// Diffuse IBL
	float3 irradiance = irradianceMap.Sample(defaultSampler, worldNormal).rgb;
	float3 diffuseIBL = irradiance * albedo.rgb;

	// Specular IBL (Assuming 1 pre-filtered mip level is bound at the top)
	float3 prefilteredColor = prefilteredMap.SampleLevel(defaultSampler, R, 0.0).rgb;
	float2 envBRDF =
		brdfLUT.Sample(defaultSampler, float2(max(dot(worldNormal, V), 0.0), roughness)).rg;
	float3 specularIBL = prefilteredColor * (F_rough * envBRDF.x + envBRDF.y);

	float3 ambient = (kD_rough * diffuseIBL + specularIBL); // Replaces flat 5% ambient!

	// Preserve exact base color alpha for BLEND transparency pipelines
	output.color = float4(ambient + directSun + directPunctual + emissive, albedo.a);

	// Calculate Motion Vectors for TAA
	float2 ndcCurr = input.currClip.xy / input.currClip.w;
	float2 ndcPrev = input.prevClip.xy / input.prevClip.w;
	output.velocity = (ndcCurr - ndcPrev) * 0.5f;

	return output;
}
