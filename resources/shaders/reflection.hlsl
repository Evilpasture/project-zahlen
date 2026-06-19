// resources/shaders/reflection.hlsl
#pragma pack_matrix(column_major)
#include <SharedMath.hpp>

[[vk::constant_id(0)]] const int ENABLE_SSR = 1;
#ifndef DISABLE_RTR
[[vk::constant_id(1)]] const int ENABLE_RTR = 1;
#else
static const int ENABLE_RTR = 0;
#endif

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

struct FrameUniforms {
	float4x4 viewProj;
	float4x4 unjitteredViewProj;
	float4x4 prevUnjitteredViewProj;
	float4x4 lightSpaceMatrix;
	float4x4 invViewProj;
	float4 camPos;
	float4 lightDir;
	uint lightCount;
	float pad0;
	float pad1;
	float pad2;
	float4 sh[9];
	float4 probeMin;
	float4 probeMax;
	float4 probePos;
	float4 jitterParams;
	int enableRTR;
	float zScale;
	float zBias;
	int rtr_pad0;
};

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

float3 EvaluateSH(float3 N, float4 sh[9]) {
	float3 result =
		sh[0].xyz * 0.282095f + sh[1].xyz * -0.488603f * N.y + sh[2].xyz * 0.488603f * N.z +
		sh[3].xyz * -0.488603f * N.x + sh[4].xyz * 1.092548f * N.x * N.y +
		sh[5].xyz * -1.092548f * N.y * N.z + sh[6].xyz * 0.315392f * (3.0f * N.z * N.z - 1.0f) +
		sh[7].xyz * -1.092548f * N.x * N.z + sh[8].xyz * 0.546274f * (N.x * N.x - N.y * N.y);
	return max(result, float3(0.0f, 0.0f, 0.0f));
}

float GetStableWeylNoise(uint2 pixelPos) {
	uint frameIndex = uint(pc.camPos.w);
	float spatial = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
	float temporal = frac(float(frameIndex % 16) * 0.6180339887498949f);
	return frac(spatial + temporal);
}

float3 UnpackNormalOctahedron(float2 oct) {
	float3 N = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
	float2 s = float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
	if (N.z < 0.0) {
		N.xy = (1.0 - abs(N.yx)) * s;
	}
	return normalize(N);
}

float3 ReconstructWorldPos(float2 uv, float depth) {
	float4 clipSpacePos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
	float4 worldSpacePos = mul(pc.invViewProj, clipSpacePos);
	return worldSpacePos.xyz / worldSpacePos.w;
}

float3 BoxParallaxCorrection(float3 posWS, float3 R, float3 boxMin, float3 boxMax,
							 float3 probePos) {
	float3 eps = float3(0.1f, 0.1f, 0.1f);
	float3 bMin = boxMin - eps;
	float3 bMax = boxMax + eps;

	float3 invR = 1.0f / max(abs(R), 0.00001f) * sign(R);
	float3 t1 = (bMax - posWS) * invR;
	float3 t2 = (bMin - posWS) * invR;
	float3 tMax = max(t1, t2);
	float distance = min(min(tMax.x, tMax.y), tMax.z);
	float3 intersectPositionWS = posWS + R * distance;
	return normalize(intersectPositionWS - probePos);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
	return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) *
					pow(saturate(1.0 - cosTheta), 5.0);
}

float2 RaymarchSSR(float3 worldPos, float3 startPosWS, float3 dirWS, float3 N,
				   out float confidence) {
	const float maxDistance = 14.0f;
	float3 endPosWS = startPosWS + dirWS * maxDistance;

	float4 startClip = mul(pc.viewProj, float4(startPosWS, 1.0f));
	float4 endClip = mul(pc.viewProj, float4(endPosWS, 1.0f));

	if (endClip.w < 0.4f) {
		float t = (0.4f - startClip.w) / (endClip.w - startClip.w);
		endPosWS = lerp(startPosWS, endPosWS, t);
		endClip = mul(pc.viewProj, float4(endPosWS, 1.0f));
	}

	float invW_start = 1.0f / startClip.w;
	float invW_end = 1.0f / endClip.w;

	float3 startNDC = startClip.xyz * invW_start;
	float3 endNDC = endClip.xyz * invW_end;

	float2 startUV = startNDC.xy * float2(0.5f, -0.5f) + 0.5f;
	float2 endUV = endNDC.xy * float2(0.5f, -0.5f) + 0.5f;

	float2 uv_w_start = startUV * invW_start;
	float2 uv_w_end = endUV * invW_end;
	float3 ws_w_start = startPosWS * invW_start;
	float3 ws_w_end = endPosWS * invW_end;

	float2 deltaUV = endUV - startUV;

	uint dw, dh;
	texDepth.GetDimensions(dw, dh);
	float2 screenPixels = abs(deltaUV * float2(dw, dh));

	float maxPixelDist = max(screenPixels.x, screenPixels.y);
	float stepCount = clamp(maxPixelDist / 4.0f, 8.0f, 48.0f);

	float invW_step = (invW_end - invW_start) / stepCount;
	float2 uv_w_step = (uv_w_end - uv_w_start) / stepCount;
	float3 ws_w_step = (ws_w_end - ws_w_start) / stepCount;

	uint2 pixelPos = uint2(startUV * float2(dw, dh));
	float dither = GetStableWeylNoise(pixelPos);

	float startOffset = 1.2f + dither;
	float current_invW = invW_start + invW_step * startOffset;
	float2 current_uv_w = uv_w_start + uv_w_step * startOffset;
	float3 current_ws_w = ws_w_start + ws_w_step * startOffset;

	confidence = 0.0f;

	[loop] for (int i = 0; i < 64; ++i) {
		if (i >= int(stepCount))
			break;

		float2 currentUV = current_uv_w / current_invW;

		if (any(currentUV < 0.0f) || any(currentUV > 1.0f))
			break;

		float sampledDepth = texDepth.SampleLevel(pointSampler, currentUV, 0).r;
		if (sampledDepth >= 1.0f) {
			current_invW += invW_step;
			current_uv_w += uv_w_step;
			current_ws_w += ws_w_step;
			continue;
		}

		float3 currentWS = current_ws_w / current_invW;
		float3 sampledWS = ReconstructWorldPos(currentUV, sampledDepth);

		float rayDist = length(currentWS - pc.camPos.xyz);
		float sampleDist = length(sampledWS - pc.camPos.xyz);
		float thickness = rayDist - sampleDist;

		if (thickness >= 0.0f && thickness < 0.4f) {

			float t_start = (float(i) + dither) / stepCount;
			float t_end = (float(i) + 1.0f + dither) / stepCount;
			t_start = max(0.0f, t_start);

			float t_mid = 0.0f;
			float2 mid_uv = 0.0f;

			[unroll(4)] for (int b = 0; b < 4; ++b) {
				t_mid = (t_start + t_end) * 0.5f;

				float mid_invW = invW_start + (invW_end - invW_start) * t_mid;
				float2 mid_uv_w = uv_w_start + (uv_w_end - uv_w_start) * t_mid;
				float3 mid_ws_w = ws_w_start + (ws_w_end - ws_w_start) * t_mid;

				mid_uv = mid_uv_w / mid_invW;
				float3 mid_ws = mid_ws_w / mid_invW;

				float midDepth = texDepth.SampleLevel(pointSampler, mid_uv, 0).r;
				float3 midSampledWS = ReconstructWorldPos(mid_uv, midDepth);

				float midRayDist = length(mid_ws - pc.camPos.xyz);
				float midSampleDist = length(midSampledWS - pc.camPos.xyz);

				if (midRayDist >= midSampleDist) {
					t_end = t_mid;
				} else {
					t_start = t_mid;
				}
			}

			float finalDepth = texDepth.SampleLevel(pointSampler, mid_uv, 0).r;
			float final_invW = invW_start + (invW_end - invW_start) * t_mid;
			float3 final_ws_w = ws_w_start + (ws_w_end - ws_w_start) * t_mid;
			float3 mid_ws = final_ws_w / final_invW;

			float3 finalSampledWS = ReconstructWorldPos(mid_uv, finalDepth);
			float finalRayDist = length(mid_ws - pc.camPos.xyz);
			float finalSampleDist = length(finalSampledWS - pc.camPos.xyz);

			float distFromStart = length(mid_ws - startPosWS);
			float heightAboveSurface = dot(mid_ws - worldPos, N);

			if (heightAboveSurface > 0.06f && abs(finalRayDist - finalSampleDist) < 0.08f) {
				float distanceFade = saturate(1.0f - distFromStart / maxDistance);
				confidence = distanceFade * distanceFade;

				float2 edgeFactor =
					smoothstep(0.0f, 0.08f, mid_uv) * smoothstep(1.0f, 0.92f, mid_uv);
				confidence *= edgeFactor.x * edgeFactor.y;
				return mid_uv;
			}
		}

		current_invW += invW_step;
		current_uv_w += uv_w_step;
		current_ws_w += ws_w_step;
	}
	return float2(0.0f, 0.0f);
}

#ifndef DISABLE_RTR
float2 RaytraceRTR(float3 worldPos, float3 N, float3 R, out float confidence) {
	confidence = 0.0f;
	RayDesc ray;
	ray.Origin = worldPos + N * 0.05f;
	ray.Direction = R;
	ray.TMin = 0.01f;
	ray.TMax = 1000.0f;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);

	while (q.Proceed()) {
	}

	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		float3 hitWorldPos = ray.Origin + ray.Direction * q.CommittedRayT();
		float4 hitClip = mul(pc.viewProj, float4(hitWorldPos, 1.0f));

		if (hitClip.w < 0.1f) {
			confidence = 0.0f;
			return float2(0.0f, 0.0f);
		}

		float2 hitNDC = hitClip.xy / hitClip.w;
		float2 hitUV = hitNDC * float2(0.5f, -0.5f) + 0.5f;

		float2 edgeFactor = smoothstep(0.0f, 0.08f, hitUV) * smoothstep(1.0f, 0.92f, hitUV);
		confidence = edgeFactor.x * edgeFactor.y;

		if (confidence > 0.0f) {
			float sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0).r;
			float3 sampledWorldPos = ReconstructWorldPos(hitUV, sampledRawDepth);

			float distToHit = length(hitWorldPos - pc.camPos.xyz);
			float distToSampled = length(sampledWorldPos - pc.camPos.xyz);

			float depthDiff = abs(distToHit - distToSampled);
			float depthMask = smoothstep(1.2f, 0.4f, depthDiff);
			confidence *= depthMask;
		}

		return hitUV;
	}
	return float2(0.0f, 0.0f);
}
#endif

struct VSOutput {
	float4 pos : SV_Position;
	float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexID : SV_VertexID) {
	VSOutput output;
	output.uv = float2((vertexID << 1) & 2, vertexID & 2);
	output.pos = float4(output.uv.x * 2.0f - 1.0f, 1.0f - output.uv.y * 2.0f, 0.0f, 1.0f);
	return output;
}

float4 PSMain(VSOutput input) : SV_Target0 {
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;
	float4 litColorRaw = texLighting.SampleLevel(pointSampler, input.uv, 0);
	if (depth >= 1.0f)
		return litColorRaw;

	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float roughness = normRoughRaw.z;
	float metallic = normRoughRaw.w;

	float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
	float3 N = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);

	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float3 R = reflect(-V, N);

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

	float3 prefilteredColor = texEnvMap.SampleLevel(smp, R_corr, roughness * 5.0f).rgb;
	float3 specularIBL = prefilteredColor * FssEss;

	// Extract AO from litColorRaw's alpha channel (written by lighting.hlsl)
	float ao = litColorRaw.a;
	float3 litColor = litColorRaw.rgb;

	// Calculate Diffuse IBL (Irradiance)
	float3 irradiance = EvaluateSH(N, frame.sh);
	float3 Favg = F0 + (1.0f - F0) / 21.0f;
	float3 FmsEms = (Favg * (1.0f - (envBRDF.x + envBRDF.y))) /
					(1.0f - Favg * (1.0f - (envBRDF.x + envBRDF.y)));
	float3 diffuseIBL = (1.0f - FssEss - FmsEms) * (1.0f - metallic) * albedoRaw.rgb * irradiance;

	// Add Diffuse IBL and Multi-Bounce energy compensation to the base lighting
	litColor += (diffuseIBL * ao) + ((FmsEms * irradiance) * ao);

	if (roughness <= 0.85f && (ENABLE_SSR != 0 || ENABLE_RTR != 0)) {
		float confidence = 0.0f;
		float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
		float2 hitUV = float2(0.0f, 0.0f);
		float3 biasedStartPos = worldPos + N * 0.05f;

		if (ENABLE_SSR != 0) {
			hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence);
		}

#ifndef DISABLE_RTR
		if (confidence < 0.1f && ENABLE_RTR != 0) {
			hitUV = RaytraceRTR(worldPos, N, R, confidence);
		}
#endif

		if (confidence > 0.0f) {
			confidence *= saturate(dot(R, N) * 10.0f);
			reflectionColor = texInput.SampleLevel(smp, hitUV, 0).rgb;
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
