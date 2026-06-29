// resources/shaders/reflection.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"
[[vk::constant_id(0)]] const int ENABLE_SSR = 1;
#ifndef DISABLE_RTR
[[vk::constant_id(1)]] const int ENABLE_RTR = 1;
#else
static const int ENABLE_RTR = 0;
#endif

float3 RotateVector(float3 V, float3 lightDir) {
	float lenB = length(lightDir);
	if (lenB < 0.001f) {
		return V;
	}
	float3 A = normalize(float3(0.5f, 1.0f, 0.2f));
	float3 B = lightDir / lenB;
	float3 q_xyz = cross(B, A);
	float q_w = 1.0f + dot(A, B);
	float q_len = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
	if (q_len > 0.0001f) {
		q_xyz /= q_len;
		q_w /= q_len;
		return V + 2.0f * cross(q_xyz, cross(q_xyz, V) + q_w * V);
	}
	return V;
}

struct PushConstants {
	float4x4 invViewProj;
	float4x4 viewProj;
	float4 camPos;
	int giMode;
	float aoRadius;
	float aoBias;
	float aoPower;
	float giIntensity;
	int giSamples;
	int enableSSR_dynamic;
	int enableRTR_dynamic;
	int _pad;
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness;
[[vk::binding(4, 0)]] SamplerState pointSampler;
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;
#ifndef DISABLE_RTR
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas;
#endif
[[vk::binding(7, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(8, 0)]] Texture2D brdfLUT;
[[vk::binding(9, 0)]] SamplerState clampSampler;
[[vk::binding(10, 0)]] Texture2D<float4> texLighting; // Lit color input from Pass 2

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);

	// Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1,
	// -1)
	output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	if (depth >= 1.0f) {
		// Reconstruct the world-space point on the far plane (depth = 1.0)
		float3 worldPos = ReconstructWorldPos(input.uv, 1.0f, pc.invViewProj);
		// Calculate the ray direction pointing away from the camera
		float3 rayDir = normalize(worldPos - pc.camPos.xyz);

		// Rotate the visual skybox to match the dynamic sun
		rayDir = RotateVector(rayDir, frame.lightDir.xyz);

		// Sample the environment map at MIP 0 with the standard 25.0f exposure boost
		float3 skyColor = texEnvMap.SampleLevel(smp, rayDir, 0.0f).rgb * 25.0f;
		return float4(skyColor, 1.0f);
	}

	float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
	if (depth >= 1.0f)
		return litColorRaw;

	if (frame.fullBright != 0) {
		return float4(litColorRaw.rgb, 1.0f);
	}

	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float roughness = normRoughRaw.z;
	float metallic = normRoughRaw.w;

	float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
	float3 N = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

	float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float3 R = reflect(-V, N);

	// Horizon-clamp: prevent perturbed normals from reflecting rays downward into the floor
	if (R.y < 0.05f) {
		R.y = 0.05f;
		R = normalize(R);
	}

	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
	float NdotV = saturate(dot(N, V));
	float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
	float3 F_rough = FresnelSchlickRoughness(NdotV, F0, roughness);
	float3 FssEss = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);

	float3 R_corr = R;
	float boxFade = 0.0f;
	if (frame.probeMin.w > 0.0f) {
		float3 boxCenter = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
		float3 boxExtent = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
		float3 distFromCenter = abs(worldPos - boxCenter);
		float normDist = max(max(abs(distFromCenter.x) / max(boxExtent.x, 0.0001f),
								 abs(distFromCenter.y) / max(boxExtent.y, 0.0001f)),
							 abs(distFromCenter.z) / max(boxExtent.z, 0.0001f));
		boxFade = smoothstep(1.0f, 0.9f, normDist);
	}

	if (boxFade > 0.0f) {
		float3 boxR = BoxParallaxCorrection(worldPos, R, frame.probeMin.xyz, frame.probeMax.xyz,
											frame.probePos.xyz);
		R_corr = lerp(R, boxR, boxFade);
	}

	// Rotate the specular lookup vector to match the dynamic sun
	R_corr = RotateVector(R_corr, frame.lightDir.xyz);

	float3 prefilteredColor =
		texEnvMap.SampleLevel(smp, R_corr, roughness * 5.0f).rgb * frame.ambientExposure;
	float3 specularIBL = prefilteredColor * FssEss;

	// Extract AO from litColorRaw's alpha channel (written by lighting.hlsl)
	float ao = litColorRaw.a;
	float3 litColor = litColorRaw.rgb;

	// Rotate the diffuse SH lookup normal to match the dynamic sun
	float3 N_rot = RotateVector(N, frame.lightDir.xyz);
	float3 irradiance = EvaluateSH(N_rot, frame.sh) * frame.ambientExposure;

	// --- ACCURATE GLOBAL IRRADIANCE OCCLUSION ---
	// Fade out the bright blue outdoor sky dome inside your indoor probe box
	if (frame.probeMin.w > 0.0f && boxFade > 0.0f) {
		float3 indoorAmbient = float3(0.12f, 0.12f, 0.12f) * frame.ambientExposure;
		irradiance = lerp(irradiance, indoorAmbient, boxFade);
	}

	// --- ENERGY COMPENSATION TERM GENERATION ---
	float3 Favg = F0 + (1.0f - F0) / 21.0f;
	float3 FmsEms = (Favg * (1.0f - (envBRDF.x + envBRDF.y))) /
					(1.0f - Favg * (1.0f - (envBRDF.x + envBRDF.y)));
	float3 diffuseIBL = (1.0f - FssEss - FmsEms) * (1.0f - metallic) * albedoRaw.rgb * irradiance;

	// Add Diffuse IBL and Multi-Bounce energy compensation to the base lighting
	litColor += (diffuseIBL * ao) + ((FmsEms * irradiance) * ao);

	if (roughness <= 0.4f && (ENABLE_SSR != 0 || ENABLE_RTR != 0)) {
		float confidence = 0.0f;
		float debugValue = 0.0f;
		float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
		float2 hitUV = float2(0.0f, 0.0f);
		float3 biasedStartPos = worldPos + N * 0.05f;

		if (ENABLE_SSR != 0) {
			// Pass debugValue to collect normalized step count
			hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence, debugValue, texDepth,
								pointSampler, pc.viewProj, pc.camPos, pc.invViewProj);
		}

#ifndef DISABLE_RTR
		if (confidence < 0.1f && ENABLE_RTR != 0) {
			// Unified Hardware RTR function from helper library
			hitUV = RaytraceRTR(worldPos, N, R, confidence, tlas, texDepth, pointSampler,
								pc.viewProj, pc.camPos, pc.invViewProj);
		}
#endif

		if (confidence > 0.0f) {
			confidence *= saturate(dot(R, N) * 10.0f);
			reflectionColor = texLighting.SampleLevel(smp, hitUV, 0).rgb;
		}

		float3 localReflection = lerp(specularIBL, reflectionColor * FssEss, confidence);

		float3 F_refl = lerp(float3(0.15f, 0.15f, 0.15f), litColor, metallic);
		float3 F_term = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
		float roughnessFade = saturate(1.0f - roughness);
		float horizonOcclusion = saturate(1.0f + dot(R, N));
		horizonOcclusion *= horizonOcclusion;

		litColor =
			lerp(litColor, litColor + localReflection * F_term * roughnessFade * horizonOcclusion,
				 roughnessFade);
	} else {
		float3 F_refl = float3(0.15f, 0.15f, 0.15f);
		float3 F_term = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
		float roughnessFade = saturate(1.0f - roughness);
		litColor = lerp(litColor, litColor + specularIBL * F_term * roughnessFade, roughnessFade);
	}

	return float4(litColor, 1.0f);
}
