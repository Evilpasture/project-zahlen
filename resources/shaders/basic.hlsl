// resources/shaders/basic.hlsl
#include "common.hlsl"

VSOutput VSMain(VSInput input, uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
	VSOutput output;

	// Resolve Instance ID: Use push constant if valid; otherwise fallback to draw SV_InstanceID
	uint instId = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
	InstanceData inst = g_instances[instId];

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

	if (isSkinned != 0) {
		worldPos =
			mul(worldMatrix, SkinPosition(localPos, input.joints, input.weights, jointOffset));
		output.normal = normalize(
			mul(world3x3, SkinDirection(localNormal, input.joints, input.weights, jointOffset)));
		output.tangent.xyz = normalize(
			mul(world3x3, SkinDirection(localTangent, input.joints, input.weights, jointOffset)));
	} else {
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
		// --- FIX: Use SkinPositionPrev instead of SkinPosition ---
		prevWorldPos = mul(prevWorldMatrix,
						   SkinPositionPrev(localPos, input.joints, input.weights, jointOffset));
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

	float4 albedo =
		globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

	if (alphaMode == 1 && albedo.a < alphaCutoff) {
		discard;
	}

	// 1. Update the normal map snapping and vertex flattening block (Around line 241)
	float3 normalMapRaw =
		globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0f - 1.0f;
	float normalLength = length(normalMapRaw);
	float3 normalMap = normalMapRaw / max(normalLength, 0.001f); // Normalize safely

	// Snap near-flat normal map pixels to absolute center to clean up texture compression noise
	if (normalMap.z > 0.998f) {
		normalMap = float3(0.0f, 0.0f, 1.0f);
	}

	float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);
	float3 emissive = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;

	// Hard clamp minimum roughness
	float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
	float metallic = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

	float3 V = normalize(frame.camPos.xyz - input.worldPos);
	float3 N = normalize(input.normal);
	float3 worldNormal = N;

	// 4. Apply Toksvig Specular Anti-Aliasing (Only if a custom normal map is bound)
	if (indices.y != 2) {
		roughness = AntiAliasRoughness(roughness, normalMapRaw);
	}

	// 5. Restore the glossy clear coat lacquer properties
	float clearcoat =
		saturate((1.0f - roughnessFactor) * 2.0f - 1.0f) * (1.0f - metallicFactor * 0.5f);
	const float clearcoatRoughness = 0.05f; // Highly glossy, flat outer shell

	// 6. Setup Orthonormal Geometric Basis for Anisotropy
	float3 T = float3(1.0f, 0.0f, 0.0f);
	float3 B = float3(0.0f, 1.0f, 0.0f);

	// Only compute tangent-space normal maps if a custom normal map is actually bound
	if (indices.y != 2 && any(input.tangent.xyz)) {
		float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
		if (dot(T_unnorm, T_unnorm) < 0.0001f) {
			T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
		}
		T = normalize(T_unnorm);
		float tangentSign = input.tangent.w * 2.0f - 1.0f;
		B = normalize(cross(N, T) * tangentSign);

		worldNormal = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
	} else {
		// Build fallback orthonormal basis
		T = normalize(cross(worldNormal, abs(worldNormal.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f)
																	 : float3(0.0f, 0.0f, 1.0f)));
		B = cross(worldNormal, T);
	}

	// --- SETUP ANISOTROPIC GGX PARAMETERS ---
	float anisotropy = 0.0f;
	if (metallic > 0.5f && roughness < 0.3f) {
		anisotropy = 0.65f;
	}

	float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo.rgb, metallic);
	float NdotV = saturate(dot(worldNormal, V));

	// -------------------------------------------------------------------------
	// CACHED TEXTURE LOOKUPS: Pull 2D BRDF LUT parameters once
	// -------------------------------------------------------------------------
	float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
	float Ev_sun = envBRDF.x + envBRDF.y;
	float Eavg_sun = GetAverageAlbedo(roughness);
	float3 Favg = F0 + (1.0f - F0) / 21.0f; // Average Fresnel term integrated over the hemisphere

	// Direct Directional (Sun) Light
	float3 L_sun = normalize(frame.lightDir.xyz);
	float3 H_sun = normalize(V + L_sun + 1e-5f);
	float NdotL_sun = saturate(dot(worldNormal, L_sun));
	float shadow = CalculateShadow(input.shadowPos, worldNormal, L_sun);

	float D = 0.0f;
	float g_term = 0.0f;

	// Optimization: Conditionally branch heavy Anisotropic math
	if (anisotropy > 0.0f) {
		float alpha = roughness * roughness;
		float alpha_x = max(alpha * (1.0f + anisotropy), 0.001f);
		float alpha_y = max(alpha * (1.0f - anisotropy), 0.001f);
		D = DistributionAnisotropicGGX(worldNormal, H_sun, T, B, alpha_x, alpha_y);
		g_term = GeometryAnisotropicSmith(worldNormal, V, L_sun, T, B, alpha_x, alpha_y);
	} else {
		D = DistributionGGX(worldNormal, H_sun, roughness);
		g_term = GeometrySmith(worldNormal, V, L_sun, roughness);
	}

	float3 F = FresnelSchlick(saturate(dot(H_sun, V)), F0);

	// 1. Single-scatter specular
	float3 spec = (D * g_term * F) / max(4.0 * NdotV * NdotL_sun, 0.001);

	// 2. Analytical multiple-scattering energy compensation (Avoid duplicate brdfLUT Samples) [2]
	float2 envBRDF_L = brdfLUT.SampleLevel(clampSampler, float2(NdotL_sun, roughness), 0.0f).rg;
	float El_sun = envBRDF_L.x + envBRDF_L.y;

	float3 Fms = (Favg * Favg * (1.0f - Eavg_sun)) / (1.0f - Favg * (1.0f - Eavg_sun));
	float3 spec_ms = ((1.0f - Ev_sun) * (1.0f - El_sun) * Fms) /
					 (3.14159265f * (1.0f - Eavg_sun) * (1.0f - Eavg_sun));
	float3 totalSpecular = spec + spec_ms;

	// 3. Energy-conserving diffuse
	float3 FmsEms_sun = (Favg * (1.0f - Ev_sun)) / (1.0f - Favg * (1.0f - Eavg_sun));
	float3 kD_sun = (1.0f - Ev_sun - FmsEms_sun) * (1.0f - metallic);

	// --- 4. CLEAR COAT spec lobe ---
	float3 spec_coat = float3(0.0f, 0.0f, 0.0f);
	float3 attenuation_coat = float3(1.0f, 1.0f, 1.0f);

	if (clearcoat > 0.01f) {
		float3 H_coat = normalize(V + L_sun + 1e-5f);
		float NdotL_coat = saturate(dot(N, L_sun)); // Uses geometric normal N
		float NdotV_coat = saturate(dot(N, V));		// Uses geometric normal N

		float D_coat = DistributionGGX(N, H_coat, clearcoatRoughness);
		float G_coat = GeometrySmith(N, V, L_sun, clearcoatRoughness);
		float3 F_coat = FresnelSchlick(saturate(dot(H_coat, V)), 0.04f);

		spec_coat = (D_coat * G_coat * F_coat) / max(4.0 * NdotV_coat * NdotL_coat, 0.001);
		attenuation_coat = 1.0f - F_coat * clearcoat;
	}

	// --- 5. Energy-Conserved Layer Blending ---
	float3 directSun = (spec_coat * clearcoat +
						attenuation_coat * (kD_sun * albedo.rgb / 3.14159265f + totalSpecular)) *
					   10.0 * NdotL_sun * shadow;

	// Only active on dielectrics (1.0 - metallic) and colored by her local albedo
	float3 sunColor = float3(1.0f, 0.95f, 0.85f) * 10.0f; // Warm direct sunlight
	float3 translucencySun =
		CalculateTranslucency(input.shadowPos, N, V, L_sun, sunColor, 0.3f, 8.0f, 0.6f);
	directSun += translucencySun * (1.0f - metallic) * albedo.rgb;

	// Process punctual and area lights in the SSBO
	float3 directPunctual = float3(0, 0, 0);
	for (uint i = 0; i < frame.lightCount; i++) {
		Light light = lights[i];

		// AREA LIGHT EVALUATION
		if (light.type == 3) {
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
				closestPoint -= R_light * min(light.radius, distToCenter);
			}

			// Recalculate L based on the new MRP surface contact point
			float3 L = normalize(closestPoint - input.worldPos);
			float NdotL = max(dot(worldNormal, L), 0.0);
			float atten = 1.0 / (distToCenter * distToCenter + 0.01); // Standard falloff

			if (NdotL > 0.0) {
				// Energy Conservation: Widen apparent roughness based on light radius
				float sphereAngle = saturate(light.radius / (distToCenter + 1e-5f));
				float modRoughness = saturate(roughness + sphereAngle * 0.5f);

				float3 H = normalize(V + L);

				float D_p = 0.0f;
				float G_p = 0.0f;
				if (anisotropy > 0.0f) {
					float alpha_p = modRoughness * modRoughness;
					float alpha_p_x = max(alpha_p * (1.0f + anisotropy), 0.001f);
					float alpha_p_y = max(alpha_p * (1.0f - anisotropy), 0.001f);
					D_p = DistributionAnisotropicGGX(worldNormal, H, T, B, alpha_p_x, alpha_p_y);
					G_p = GeometryAnisotropicSmith(worldNormal, V, L, T, B, alpha_p_x, alpha_p_y);
				} else {
					D_p = DistributionGGX(worldNormal, H, modRoughness);
					G_p = GeometrySmith(worldNormal, V, L, modRoughness);
				}

				float3 F_p = FresnelSchlick(max(dot(H, V), 0.0), F0);

				// 1. Single-scatter specular
				float3 spec_p =
					(D_p * G_p * F_p) / (4.0 * max(dot(worldNormal, V), 0.0) * NdotL + 0.0001);

				// -------------------------------------------------------------
				// FAST-PATH: Bypassed heavy multiple-scattering loops for punctual lights [2]
				// -------------------------------------------------------------
				float3 totalSpecular_p = spec_p;
				float3 kD_p = (1.0f - F_p) * (1.0f - metallic);

				// --- 4. CLEAR COAT spec lobe ---
				float3 spec_coat_p = float3(0.0f, 0.0f, 0.0f);
				float3 attenuation_coat_p = float3(1.0f, 1.0f, 1.0f);

				if (clearcoat > 0.01f) {
					float3 H_coat = normalize(V + L);
					float NdotL_coat = saturate(dot(N, L)); // Geometric normal
					float NdotV_coat = saturate(dot(N, V)); // Geometric normal

					float D_coat = DistributionGGX(N, H_coat, clearcoatRoughness);
					float G_coat = GeometrySmith(N, V, L, clearcoatRoughness);
					float3 F_coat = FresnelSchlick(saturate(dot(H_coat, V)), 0.04f);

					spec_coat_p =
						(D_coat * G_coat * F_coat) / max(4.0 * NdotV_coat * NdotL_coat, 0.001);
					attenuation_coat_p = 1.0f - F_coat * clearcoat;
				}

				// --- 5. Energy-Conserved Layer Blending ---
				directPunctual +=
					(spec_coat_p * clearcoat +
					 attenuation_coat_p * (kD_p * albedo.rgb / 3.14159265f + totalSpecular_p)) *
					light.color * light.intensity * atten * NdotL;
			} else {
				// --- 6. Backlit Translucency for Point & Spot Lights ---
				float3 H_p = normalize(L + N * 0.3f); // Use geometric normal N
				float dotVH_p = saturate(dot(V, -H_p));

				float thicknessScale = 0.25f;
				float3 translucencyPunctual = light.color * light.intensity * atten *
											  pow(dotVH_p, 8.0f) * 0.4f * thicknessScale;

				directPunctual += translucencyPunctual * (1.0f - metallic) * albedo.rgb;
			}
		}
	}

	// --- NEW: FULL PBR INDIRECT IMAGE-BASED LIGHTING (IBL) ---
	float3 R = reflect(-V, worldNormal);
	float3 F_rough = FresnelSchlickRoughness(max(dot(worldNormal, V), 0.0), F0, roughness);

	// 1. Evaluate SH Diffuse
	float3 irradiance = EvaluateSH(worldNormal, frame.sh);

	// --- PRE-CALCULATE BOX PROJECTION BOUNDARY FADE ---
	float boxFade = 0.0f;
	if (frame.probeMin.w > 0.0f) {
		float3 boxCenter = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
		float3 boxExtent = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
		float3 distFromCenter = abs(input.worldPos - boxCenter);
		float3 normDist = distFromCenter / max(boxExtent, 0.0001f);
		float maxDist = max(max(normDist.x, normDist.y), normDist.z);

		boxFade = smoothstep(1.0f, 0.9f, maxDist);
	}

	// 2. Evaluate Single-Scatter Specular IBL (With Lobe Elongation & Blended Parallax Correction)

	float3 correctedR = R;
	if (boxFade > 0.0f) {
		float3 boxR = BoxParallaxCorrection(input.worldPos, R, frame.probeMin.xyz,
											frame.probeMax.xyz, frame.probePos.xyz);
		correctedR = lerp(R, boxR, boxFade);
	}

	float maxMipLevel = 5.0f;
	float3 prefilteredColor =
		prefilteredMap.SampleLevel(clampSampler, correctedR, roughness * maxMipLevel).rgb;

	// Use our single pre-sampled envBRDF texture fetch instead of duplicate lookups [2]
	float3 FssEss = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);
	float3 specularIBL = prefilteredColor * FssEss;

	// 3. Multi-Scatter Energy Compensation (Kulla-Conty / Fdez-Agüera)
	float Ess = envBRDF.x + envBRDF.y;
	float Ems = 1.0f - Ess;
	float3 FmsEms = (Favg * Ems) / (1.0f - Favg * Ems);
	float3 multiScatterIBL = FmsEms * irradiance;

	// 4. Energy-Conserving Diffuse
	float3 kD_IBL = (1.0f - FssEss - FmsEms) * (1.0f - metallic);
	float3 diffuseIBL = kD_IBL * albedo.rgb * irradiance;

	// --- 5. CLEAR COAT SPECULAR IBL (With Blended Parallax Correction) ---
	float3 clearcoatSpecularIBL = float3(0.0f, 0.0f, 0.0f);
	float3 baseIBLAttenuation = float3(1.0f, 1.0f, 1.0f);

	if (clearcoat > 0.01f) {
		float3 R_geom = reflect(-V, N); // Flat geometric reflection (no normal map bumps)
		float3 correctedR_coat = R_geom;
		if (boxFade > 0.0f) {
			float3 boxR_coat = BoxParallaxCorrection(input.worldPos, R_geom, frame.probeMin.xyz,
													 frame.probeMax.xyz, frame.probePos.xyz);
			correctedR_coat = lerp(R_geom, boxR_coat, boxFade);
		}
		float3 prefilteredCoatColor =
			prefilteredMap
				.SampleLevel(clampSampler, correctedR_coat, clearcoatRoughness * maxMipLevel)
				.rgb;
		float3 F_coat_IBL = FresnelSchlickRoughness(max(dot(N, V), 0.0), 0.04f, clearcoatRoughness);
		clearcoatSpecularIBL = prefilteredCoatColor * F_coat_IBL * clearcoat;
		baseIBLAttenuation = 1.0f - F_coat_IBL * clearcoat;
	}

	// --- 6. Final Layer Blending ---
	float3 ambient =
		clearcoatSpecularIBL + baseIBLAttenuation * (diffuseIBL + specularIBL + multiScatterIBL);

	// Combine all lighting
	float3 finalLight = ambient + directSun + directPunctual + emissive;
	finalLight = min(finalLight, 100.0f); // TAA register overflow safeguard

	output.color = float4(finalLight, albedo.a);
	output.normalRoughness = float4(worldNormal * 0.5f + 0.5f, roughness);

	// Clamp W to a small positive number to prevent Divide-By-Zero NaN explosions
	float currW = max(input.currClip.w, 0.0001f);
	float prevW = max(input.prevClip.w, 0.0001f);

	float2 ndcCurr = input.currClip.xy / currW;
	float2 ndcPrev = input.prevClip.xy / prevW;
	output.velocity = (ndcCurr - ndcPrev) * float2(0.5f, -0.5f);

	return output;
}
