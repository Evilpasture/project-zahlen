// resources/shaders/pbr_helpers.hlsl
#pragma once
#include <SharedMath.hpp>

// ============================================================================
// CONSTANTS & NOISE
// ============================================================================
static const float3 HemisphereSamples[8] = {
	float3(0.35517f, -0.06385f, 0.93261f), float3(-0.19134f, 0.37512f, 0.90695f),
	float3(0.52841f, 0.50284f, 0.68412f),  float3(-0.67215f, -0.32111f, 0.66723f),
	float3(0.11211f, -0.78121f, 0.61432f), float3(-0.73211f, 0.42152f, 0.53512f),
	float3(0.81232f, -0.31211f, 0.49312f), float3(0.05211f, 0.88121f, 0.47012f)};

static const float2 PoissonDisk[16] = {
	float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725),
	float2(-0.094184101, -0.9293887), float2(0.34495938, 0.2938776),
	float2(-0.91588581, 0.45771432),  float2(-0.81544232, -0.87912464),
	float2(-0.38208752, 0.27676845),  float2(0.97484398, 0.7564837),
	float2(0.44323325, -0.97511554),  float2(0.53742981, -0.4737342),
	float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
	float2(-0.2418884, 0.99706507),	  float2(-0.81409955, -0.9143759),
	float2(0.19984126, 0.78641367),	  float2(0.14383161, -0.1410079)};

float GetStableWeylNoise(uint2 pixelPos, float timeOffset) {
	float spatial = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
	float temporal = frac(timeOffset * 0.6180339887498949f);
	return frac(spatial + temporal);
}

float GetRotationAngle(float2 screenPos, float timeOffset) {
	float temporalOffset = timeOffset * 1.61803398875f * 10.0f;
	screenPos += temporalOffset;
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

float GetShadowDither(float2 screenPos) {
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

// ============================================================================
// MATH UTILITIES
// ============================================================================
float3 UnpackNormalOctahedron(float2 oct) {
	float3 N = float3(oct, 1.0 - abs(oct.x) - abs(oct.y));
	float2 s = float2(N.x >= 0.0 ? 1.0 : -1.0, N.y >= 0.0 ? 1.0 : -1.0);
	if (N.z < 0.0) {
		N.xy = (1.0 - abs(N.yx)) * s;
	}
	return normalize(N);
}

float3 ReconstructWorldPos(float2 uv, float depth, float4x4 invViewProj) {
	float4 clipSpacePos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
	float4 worldSpacePos = mul(invViewProj, clipSpacePos);
	return worldSpacePos.xyz / worldSpacePos.w;
}

float SoftClamp(float x, float limit) {
	return limit * (1.0f - exp(-x / max(limit, 0.0001f)));
}

// ============================================================================
// PERCENTAGE-CLOSER SOFT SHADOWS (PCSS)
// ============================================================================
#define BLOCKER_SEARCH_SAMPLES 16
#define PCF_SAMPLES 16

// FIX: Update shadowMap type to Texture2D<float> and remove vector .r swizzle
float FindBlocker(float2 uv, float zReceiver, float searchRadius, Texture2D<float> shadowMap,
				  SamplerState linearSampler) {
	float blockerSum = 0.0f;
	int numBlockers = 0;

	[unroll] for (int i = 0; i < BLOCKER_SEARCH_SAMPLES; ++i) {
		float2 offset = PoissonDisk[i] * searchRadius;
		float shadowMapDepth = shadowMap.SampleLevel(linearSampler, uv + offset, 0); // Removed .r
		if (shadowMapDepth < zReceiver) {
			blockerSum += shadowMapDepth;
			numBlockers++;
		}
	}

	if (numBlockers == 0)
		return -1.0f;
	return blockerSum / (float)numBlockers;
}

// FIX: Update shadowMap type to Texture2D<float> and remove SampleCmp .r swizzle
float CalculateShadowPCSS(float4 shadowPos, float3 N, float3 L, float2 screenPos,
						  Texture2D<float> shadowMap, SamplerComparisonState shadowSampler,
						  SamplerState linearSampler) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;

	// Bounds check
	if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
		projCoords.z < 0.0 || projCoords.z > 1.0) {
		return 1.0f;
	}

	float zReceiver = projCoords.z;
	float texelSize = 1.0 / 2048.0;

	// FIX: Reduced depth bias to 0.0003 (max ~12cm at 400m range) to prevent Peter Panning / Light
	// Leaks
	float bias = max(0.0003 * (1.0 - dot(N, L)), 0.0001);
	zReceiver -= bias;

	// 1. Blocker Search
	float searchRadius = 4.0 * texelSize;
	float avgBlockerDepth =
		FindBlocker(projCoords.xy, zReceiver, searchRadius, shadowMap, linearSampler);

	// No blockers found -> fully illuminated
	if (avgBlockerDepth == -1.0f)
		return 1.0f;

	// 2. Penumbra Estimation (Orthographic)
	float worldDepthDiff = (zReceiver - avgBlockerDepth) * 400.0f;

	// FIX: Increased softness multiplier from 0.15f to 0.5f for softer edges
	float penumbraWidth = worldDepthDiff * 0.5f;

	// FIX: Enforce a minimum 2.5 texel blur radius to hide the shadow map's pixel grid
	float filterRadius = clamp(penumbraWidth, 2.5f, 16.0f) * texelSize;

	// 3. Filtered PCF pass (Rotated Poisson Disk)
	float shadow = 0.0f;
	float angle = GetShadowDither(screenPos) * 2.0 * 3.14159265;
	float s = sin(angle);
	float c = cos(angle);

	[unroll] for (int i = 0; i < PCF_SAMPLES; ++i) {
		float2 offset = float2(PoissonDisk[i].x * c - PoissonDisk[i].y * s,
							   PoissonDisk[i].x * s + PoissonDisk[i].y * c) *
						filterRadius;

		shadow += shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy + offset,
											   zReceiver); // Removed .r
	}

	return shadow / (float)PCF_SAMPLES;
}

// ============================================================================
// ANALYTICAL PBR LIGHTING
// ============================================================================
float DistributionGGX(float3 N, float3 H, float roughness) {
	float a = roughness * roughness;
	float a2 = a * a;
	float NdotH = saturate(dot(N, H));
	float denom = (NdotH * NdotH * (a2 - 1.0f) + 1.0f);
	return a2 / (3.14159265f * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness) {
	float k = pow(roughness + 1.0f, 2.0f) / 8.0f;
	return NdotV / max(NdotV * (1.0f - k) + k, 0.001f);
}

float GeometrySmith(float3 N, float3 V, float3 L, float roughness) {
	return GeometrySchlickGGX(saturate(dot(N, V)), roughness) *
		   GeometrySchlickGGX(saturate(dot(N, L)), roughness);
}

float3 FresnelSchlick(float cosTheta, float3 F0) {
	return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
	return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) *
					pow(saturate(1.0 - cosTheta), 5.0);
}

float GetDirectionalAlbedo(float NoX, float roughness, Texture2D brdfLUT,
						   SamplerState clampSampler) {
	float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(saturate(NoX), roughness), 0.0f).rg;
	return envBRDF.x + envBRDF.y;
}

float GetAverageAlbedo(float roughness) {
	return 1.0f - roughness * (0.334f - roughness * 0.125f);
}

// ============================================================================
// IBL & GLOBAL ILLUMINATION
// ============================================================================
float3 EvaluateSH(float3 N, float4 sh[9]) {
	float3 result =
		sh[0].xyz * 0.282095f + sh[1].xyz * -0.488603f * N.y + sh[2].xyz * 0.488603f * N.z +
		sh[3].xyz * -0.488603f * N.x + sh[4].xyz * 1.092548f * N.x * N.y +
		sh[5].xyz * -1.092548f * N.y * N.z + sh[6].xyz * 0.315392f * (3.0f * N.z * N.z - 1.0f) +
		sh[7].xyz * -1.092548f * N.x * N.z + sh[8].xyz * 0.546274f * (N.x * N.x - N.y * N.y);
	return max(result, float3(0.0f, 0.0f, 0.0f));
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

// ============================================================================
// AREA LIGHTS (LTC)
// ============================================================================
float3 IntegrateEdgeVector(float3 v1, float3 v2) {
	float x = dot(v1, v2);
	float y = abs(x);
	float a = 0.8543985 + (0.4965155 + 0.0145206 * y) * y;
	float b = 3.4175940 + (4.1616724 + y) * y;
	float v = a / b;
	float theta_sintheta = (x > 0.0) ? v : 0.5 * rsqrt(max(1.0 - x * x, 1e-7)) - v;
	return cross(v1, v2) * theta_sintheta;
}

float3 LTC_Evaluate(float3 N, float3 V, float3 P, float3x3 Minv, float4 points[4], bool twoSided) {
	float3 T1, T2;
	T1 = normalize(V - N * dot(V, N));
	T2 = cross(N, T1);
	float3x3 R = float3x3(T1, T2, N);

	float3 L[5];
	L[0] = mul(Minv, mul(R, points[0].xyz - P));
	L[1] = mul(Minv, mul(R, points[1].xyz - P));
	L[2] = mul(Minv, mul(R, points[2].xyz - P));
	L[3] = mul(Minv, mul(R, points[3].xyz - P));

	int n = 0;
	float3 clipped[5];

	[unroll] for (int i = 0; i < 4; ++i) {
		float3 v1 = L[i];
		float3 v2 = L[(i + 1) % 4];

		if (v1.z > 0.0) {
			clipped[n++] = v1;
		}
		if ((v1.z > 0.0 && v2.z < 0.0) || (v1.z < 0.0 && v2.z > 0.0)) {
			float t = v1.z / (v1.z - v2.z);
			clipped[n++] = lerp(v1, v2, t);
		}
	}

	if (n < 3)
		return float3(0, 0, 0);
	[unroll] for (int j = 0; j < n; ++j) {
		clipped[j] = normalize(clipped[j]);
	}
	float3 sum = float3(0, 0, 0);
	sum += IntegrateEdgeVector(clipped[0], clipped[1]);
	sum += IntegrateEdgeVector(clipped[1], clipped[2]);
	if (n >= 4)
		sum += IntegrateEdgeVector(clipped[2], clipped[3]);
	if (n == 5)
		sum += IntegrateEdgeVector(clipped[3], clipped[4]);

	sum = twoSided ? abs(sum) : max(float3(0, 0, 0), sum);
	return sum;
}

// ============================================================================
// REFLECTIONS
// ============================================================================
float2 RaymarchSSR(float3 worldPos, float3 startPosWS, float3 dirWS, float3 N, out float confidence,
				   Texture2D<float> texDepth, SamplerState pointSampler, float4x4 viewProj,
				   float4 camPos, float4x4 invViewProj) {
	const float maxDistance = 14.0f;
	float3 endPosWS = startPosWS + dirWS * maxDistance;

	float4 startClip = mul(viewProj, float4(startPosWS, 1.0f));
	float4 endClip = mul(viewProj, float4(endPosWS, 1.0f));

	if (endClip.w < 0.4f) {
		float t = (0.4f - startClip.w) / (endClip.w - startClip.w);
		endPosWS = lerp(startPosWS, endPosWS, t);
		endClip = mul(viewProj, float4(endPosWS, 1.0f));
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
	float dither = GetStableWeylNoise(pixelPos, camPos.w);

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
		float3 sampledWS = ReconstructWorldPos(currentUV, sampledDepth, invViewProj);

		float rayDist = length(currentWS - camPos.xyz);
		float sampleDist = length(sampledWS - camPos.xyz);
		float thickness = rayDist - sampleDist;

		if (thickness >= 0.0f && thickness < 0.4f) {
			float t_start = max(0.0f, (float(i) + dither) / stepCount);
			float t_end = (float(i) + 1.0f + dither) / stepCount;

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
				float3 midSampledWS = ReconstructWorldPos(mid_uv, midDepth, invViewProj);

				float midRayDist = length(mid_ws - camPos.xyz);
				float midSampleDist = length(midSampledWS - camPos.xyz);

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

			float3 finalSampledWS = ReconstructWorldPos(mid_uv, finalDepth, invViewProj);
			float finalRayDist = length(mid_ws - camPos.xyz);
			float finalSampleDist = length(finalSampledWS - camPos.xyz);

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
float2 RaytraceRTR(float3 worldPos, float3 N, float3 R, out float confidence,
				   RaytracingAccelerationStructure tlas, Texture2D<float> texDepth,
				   SamplerState pointSampler, float4x4 viewProj, float4 camPos,
				   float4x4 invViewProj) {
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
		float4 hitClip = mul(viewProj, float4(hitWorldPos, 1.0f));

		if (hitClip.w < 0.1f)
			return float2(0.0f, 0.0f);

		float2 hitNDC = hitClip.xy / hitClip.w;
		float2 hitUV = hitNDC * float2(0.5f, -0.5f) + 0.5f;

		float2 edgeFactor = smoothstep(0.0f, 0.08f, hitUV) * smoothstep(1.0f, 0.92f, hitUV);
		confidence = edgeFactor.x * edgeFactor.y;

		if (confidence > 0.0f) {
			float sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0).r;
			float3 sampledWorldPos = ReconstructWorldPos(hitUV, sampledRawDepth, invViewProj);

			float distToHit = length(hitWorldPos - camPos.xyz);
			float distToSampled = length(sampledWorldPos - camPos.xyz);

			float depthDiff = abs(distToHit - distToSampled);
			float depthMask = smoothstep(1.2f, 0.4f, depthDiff);
			confidence *= depthMask;
		}
		return hitUV;
	}
	return float2(0.0f, 0.0f);
}
#endif
