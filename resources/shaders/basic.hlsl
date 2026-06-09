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

	if (activeMorphCount > 0) {
		localPos.xyz += GetMorphDisplacement(vertexId, vertexCount, morphOffset, activeMorphCount,
											 morphWeights);
	}

	float4 worldPos;
	float3x3 world3x3 = (float3x3)worldMatrix;

	// 2. Select transform path based on skinning flag
	if (isSkinned != 0) {
		// Skinned: Bone matrices transform vertices to model space, then we multiply by worldMatrix
		// to go to world space [6]
		worldPos =
			mul(worldMatrix, SkinPosition(localPos, input.joints, input.weights, jointOffset));
		output.normal = normalize(
			mul(world3x3, SkinDirection(localNormal, input.joints, input.weights, jointOffset)));
		output.tangent.xyz = normalize(
			mul(world3x3, SkinDirection(localTangent, input.joints, input.weights, jointOffset)));
	} else {
		// Non-skinned: Transform normally using the node's worldMatrix
		worldPos = mul(worldMatrix, localPos);
		output.normal = normalize(mul(world3x3, localNormal));
		output.tangent.xyz = normalize(mul(world3x3, localTangent));
	}

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

	// Render normally using the Jittered projection
	output.pos = mul(frame.viewProj, worldPos);

	// Evaluate velocity using Unjittered projections
	output.currClip = mul(frame.unjitteredViewProj, worldPos);

	float4 prevWorldPos;
	if (isSkinned != 0) {
		// Transform the skinned position using the previous frame's world matrix
		prevWorldPos =
			mul(prevWorldMatrix, SkinPosition(localPos, input.joints, input.weights, jointOffset));
	} else {
		prevWorldPos = mul(prevWorldMatrix, localPos);
	}
	output.prevClip = mul(frame.prevUnjitteredViewProj, prevWorldPos);

	// Removed the normal/tangent overwrite here since they are calculated in the
	// skinned/non-skinned branch above!
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
	float NdotH = saturate(dot(N, H));
	float denom = (NdotH * NdotH * (a2 - 1.0) + 1.0);

	// Reverted to pure math. No artificial clamps crushing the peak!
	return a2 / (3.14159 * denom * denom);
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

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0 - 1.0;
	float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);
	float3 emissive = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;

	// FIX: Hard clamp minimum roughness to Frostbite's 0.045 to prevent GGX NaN explosions
	float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
	float metallic = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	if (any(input.tangent.xyz)) {
		float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
		if (dot(T_unnorm, T_unnorm) < 0.0001f) {
			T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
		}
		float3 T = normalize(T_unnorm);
		float tangentSign = input.tangent.w * 2.0f - 1.0f;
		float3 B = normalize(cross(N, T) * tangentSign);

		worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
	}

	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);

	// Direct Directional (Sun) Light
	float3 L_sun = normalize(frame.lightDir.xyz); // Ensure no minus sign!
	float3 H_sun = normalize(V + L_sun + 1e-5f);
	float NdotL_sun = saturate(dot(worldNormal, L_sun));
	float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);

	float D = DistributionGGX(worldNormal, H_sun, roughness);
	float g_term = GeometrySmith(worldNormal, V, L_sun, roughness);
	float3 F = FresnelSchlick(saturate(dot(H_sun, V)), F0);

	float3 spec = (D * g_term * F) / max(4.0 * saturate(dot(worldNormal, V)) * NdotL_sun, 0.001);
	float3 kD = (1.0 - F) * (1.0 - metallic);
	float3 directSun = (kD * albedo.rgb / 3.14159 + spec) * 10.0 * NdotL_sun * shadow;

	// Process punctual and area lights in the SSBO
	float3 directPunctual = float3(0, 0, 0);
	for (uint i = 0; i < frame.lightCount; i++) {
		Light light = lights[i];

		// AREA LIGHT EVALUATION
		if (light.type == 3) {
			float NdotV = saturate(dot(worldNormal, V));
			float2 uv = float2(roughness, sqrt(1.0f - NdotV));

			// Reconstruct LTC Matrix
			float4 t1 = ltc_mat.SampleLevel(clampSampler, uv, 0.0f);
			float3x3 Minv = float3x3(float3(t1.x, 0.0f, t1.y), float3(0.0f, 1.0f, 0.0f),
									 float3(t1.z, 0.0f, t1.w));

			// Reconstruct amplitude and fresnel
			float4 t2 = ltc_amp.SampleLevel(clampSampler, uv, 0.0f);
			float2 schlick = float2(t2.x, t2.y);

			// Specular Evaluation
			float3 specLTC = LTC_Evaluate(worldNormal, V, input.worldPos, Minv, light.points,
										  light.twoSided == 1);
			specLTC *= F0 * schlick.x + (1.0 - F0) * schlick.y;

			// Diffuse Evaluation (Identity Matrix approximation)
			float3 diffLTC =
				LTC_Evaluate(worldNormal, V, input.worldPos, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1),
							 light.points, light.twoSided == 1);
			float3 kD_LTC = (1.0 - metallic);

			directPunctual +=
				(kD_LTC * albedo.rgb * diffLTC + specLTC) * light.color * light.intensity;
		} else {
			// Base distance vectors
			float3 L_unnorm = light.position - input.worldPos;
			float distToCenter = length(L_unnorm);
			float3 L_center = L_unnorm / (distToCenter + 1e-5f);

			// --- MOST REPRESENTATIVE POINT (MRP) APPROXIMATION ---
			float3 closestPoint = light.position;
			if (light.radius > 0.0f) {
				float3 R_light = reflect(-V, worldNormal);
				// Move light position strictly toward the reflection ray
				closestPoint -= R_light * min(light.radius, distToCenter);
			}

			// Recalculate L based on the new MRP surface contact point
			float3 L = normalize(closestPoint - input.worldPos);
			float NdotL = max(dot(worldNormal, L), 0.0);
			float atten = 1.0 / (distToCenter * distToCenter +
								 0.01); // Standard falloff (using actual center distance)

			if (NdotL > 0.0) {
				// Energy Conservation: Widen apparent roughness based on light radius
				float sphereAngle = saturate(light.radius / (distToCenter + 1e-5f));
				float modRoughness = saturate(roughness + sphereAngle * 0.5f);

				float3 H = normalize(V + L);
				float D_p = DistributionGGX(worldNormal, H, modRoughness);
				float G_p = GeometrySmith(worldNormal, V, L, modRoughness);
				float3 F_p = FresnelSchlick(max(dot(H, V), 0.0), F0);

				float3 spec_p =
					(D_p * G_p * F_p) / (4.0 * max(dot(worldNormal, V), 0.0) * NdotL + 0.0001);
				float3 kD_p = (1.0 - F_p) * (1.0 - metallic);

				directPunctual += (kD_p * albedo.rgb / 3.14159 + spec_p) * light.color *
								  light.intensity * atten * NdotL;
			}
		}
	}
	// --- NEW: FULL PBR INDIRECT IMAGE-BASED LIGHTING (IBL) ---
	float3 R = reflect(-V, worldNormal);
	float3 F_rough = FresnelSchlickRoughness(max(dot(worldNormal, V), 0.0), F0, roughness);
	float3 kS_rough = F_rough;
	float3 kD_rough = (1.0 - kS_rough) * (1.0 - metallic);

	// Diffuse IBL
	float3 irradiance = irradianceMap.Sample(clampSampler, worldNormal).rgb;
	float3 diffuseIBL = irradiance * albedo.rgb;

	// Specular IBL (6 Mip Levels: 0.0 to 5.0)
	float maxMipLevel = 5.0f;
	float3 prefilteredColor =
		prefilteredMap.SampleLevel(clampSampler, R, roughness * maxMipLevel).rgb;
	float2 envBRDF =
		brdfLUT.Sample(clampSampler, float2(max(dot(worldNormal, V), 0.0), roughness)).rg;
	float3 specularIBL = prefilteredColor * (F_rough * envBRDF.x + envBRDF.y);
	float3 ambient = (kD_rough * diffuseIBL + specularIBL);

	// Combine all lighting
	float3 finalLight = ambient + directSun + directPunctual + emissive;

	// FIX: Clamp HDR peak to 100.0. This is blistering bright, but safely
	// prevents (c * c) Infinity overflows inside the 17-bit TAA registers.
	finalLight = min(finalLight, 100.0f);

	output.color = float4(finalLight, albedo.a);
	output.normalRoughness = float4(worldNormal * 0.5f + 0.5f, roughness);

	// Clamp W to a small positive number to prevent Divide-By-Zero NaN explosions
	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);

	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);
	// --- output.color overrides ---
	// output.color = float4(envBRDF.rg, 0.0f, 1.0f);
	// output.color = float4(worldNormal * 0.5f + 0.5f, 1.0f);
	// output.color = float4(N * 0.5f + 0.5f, 1.0f);
	// output.color = float4(B * 0.5f + 0.5f, 1.0f);
	//	output.color = float4(abs(currW - prevW) * 100.0f, 0.0f, 0.0f, 1.0f);
	//	output.color = float4(R * 0.5f + 0.5f, 1.0f);
	//	output.color = float4(specularIBL, 1.0f);
	//	output.color = float4(F_rough, 1.0f);
	//	output.color = float4(float3(D, D, D) * 0.01f, 1.0f); // Scaled down to fit display
	// output.color = float4(float3(g_term, g_term, g_term), 1.0f);
	//	output.color = float4(roughness, roughness, roughness, 1.0f);
	//	output.color = float4(metallic, metallic, metallic, 1.0f);
	//	output.color = float4(shadow, shadow, shadow, 1.0f);
	// ------------------------------
	return output;
}
