// resources/shaders/postprocess.hlsl
#pragma pack_matrix(column_major)
#include <SharedMath.hpp>

#ifdef __cplusplus
#define HLSL_LOOP
#define HLSL_UNROLL
#define HLSL_UNROLL_N(x)
#define OUT_REF(type) type&
#else
#define HLSL_LOOP [loop]
#define HLSL_UNROLL [unroll]
#define HLSL_UNROLL_N(x) [unroll(x)]
#define OUT_REF(type) out type
#endif

#ifndef __cplusplus

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
	int enableSSR;
	int enableRTR;
	int _pad;
};
[[vk::push_constant]] PushConstants pc;

// --- DITHERING & HEMISPHERE SAMPLERS ---
static const float3 HemisphereSamples[8] = {
	float3(0.35517f, -0.06385f, 0.93261f), float3(-0.19134f, 0.37512f, 0.90695f),
	float3(0.52841f, 0.50284f, 0.68412f),  float3(-0.67215f, -0.32111f, 0.66723f),
	float3(0.11211f, -0.78121f, 0.61432f), float3(-0.73211f, 0.42152f, 0.53512f),
	float3(0.81232f, -0.31211f, 0.49312f), float3(0.05211f, 0.88121f, 0.47012f)};

float GetRotationAngle(float2 screenPos) {
	uint frameIndex = uint(pc.camPos.w);
	float temporalOffset = float(frameIndex % 16) * 1.61803398875f * 10.0f;
	screenPos += temporalOffset;
	float3 magic = float3(0.06711056f, 0.00583715f, 52.9829189f);
	return frac(magic.z * frac(dot(screenPos, magic.xy)));
}

float GetStableWeylNoise(uint2 pixelPos) {
	uint frameIndex = uint(pc.camPos.w);
	float spatial = frac(float(pixelPos.x * 12664589 + pixelPos.y * 9546283) * 0.6180339887498949f);
	float temporal = frac(float(frameIndex % 16) * 0.6180339887498949f);
	return frac(spatial + temporal);
}

[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness;
[[vk::binding(4, 0)]] SamplerState pointSampler;
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;
#ifndef DISABLE_RTR
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas;
#endif

float3 ReconstructWorldPos(float2 uv, float depth) {
	float4 clipSpacePos = float4(uv.x * 2.0f - 1.0f, (1.0f - uv.y) * 2.0f - 1.0f, depth, 1.0f);
	float4 worldSpacePos = mul(pc.invViewProj, clipSpacePos);
	return worldSpacePos.xyz / worldSpacePos.w;
}

float2 RaymarchSSR(float3 worldPos, float3 startPosWS, float3 dirWS, float3 N,
				   OUT_REF(float) confidence) {
	const float maxDistance = 14.0f; // Limit trace distance for tight, realistic indoor reflections
	float3 endPosWS = startPosWS + dirWS * maxDistance;

	float4 startClip = mul(pc.viewProj, float4(startPosWS, 1.0f));
	float4 endClip = mul(pc.viewProj, float4(endPosWS, 1.0f));

	// 1. Homogeneous near-plane clipping with a safer margin (W = 0.4)
	// Prevents coordinate glitching and keeps reflections active when looking down
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

	// Dynamic Pixel-Stride Stepping (higher density at 4 pixels per step)
	float maxPixelDist = max(screenPixels.x, screenPixels.y);
	float stepCount = clamp(maxPixelDist / 4.0f, 16.0f, 64.0f);

	float invW_step = (invW_end - invW_start) / stepCount;
	float2 uv_w_step = (uv_w_end - uv_w_start) / stepCount;
	float3 ws_w_step = (ws_w_end - ws_w_start) / stepCount;

	uint2 pixelPos = uint2(startUV * float2(dw, dh));
	float dither = GetStableWeylNoise(pixelPos);

	// Initial offset of 1.2 steps + dither to completely clear the starting pixel
	float startOffset = 1.2f + dither;
	float current_invW = invW_start + invW_step * startOffset;
	float2 current_uv_w = uv_w_start + uv_w_step * startOffset;
	float3 current_ws_w = ws_w_start + ws_w_step * startOffset;

	confidence = 0.0f;

	HLSL_LOOP for (int i = 0; i < 64; ++i) {
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

		// Tight coarse thickness threshold to prevent leaks
		if (thickness >= 0.0f && thickness < 0.4f) {

			float t_start = (float(i) + dither) / stepCount;
			float t_end = (float(i) + 1.0f + dither) / stepCount;
			t_start = max(0.0f, t_start);

			float t_mid = 0.0f;
			float2 mid_uv = 0.0f;

			// 2. INCREASED BINARY SEARCH STEPS (6 steps for ultra-high sub-pixel precision)
			HLSL_UNROLL_N(6) for (int b = 0; b < 6; ++b) {
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

			// 3. Tighten height above surface & depth match to eliminate self-reflection
			if (heightAboveSurface > 0.06f && abs(finalRayDist - finalSampleDist) < 0.08f) {

				// 4. Quadratic Falloff for beautiful, smooth fade-out (Stops vertical elongation)
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
float2 RaytraceRTR(float3 worldPos, float3 N, float3 R, OUT_REF(float) confidence) {
	confidence = 0.0f;
	RayDesc ray;
	ray.Origin = worldPos + N * 0.05f; // Bias ray to prevent shadow acne self-intersection
	ray.Direction = R;
	ray.TMin = 0.01f;
	ray.TMax = 1000.0f;

	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);

	// DXR handles opaque hits internally. Proceed() returns false when traversal is complete.
	while (q.Proceed()) {
	}

	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		float3 hitWorldPos = ray.Origin + ray.Direction * q.CommittedRayT();
		float4 hitClip = mul(pc.viewProj, float4(hitWorldPos, 1.0f));

		// --- GUARD: Reject hits behind the near plane ---
		if (hitClip.w < 0.1f) {
			confidence = 0.0f;
			return float2(0.0f, 0.0f);
		}
		// ------------------------------------------------

		float2 hitNDC = hitClip.xy / hitClip.w;
		float2 hitUV = hitNDC * float2(0.5f, -0.5f) + 0.5f;

		// Soft falloff at screen boundaries
		float2 edgeFactor = smoothstep(0.0f, 0.08f, hitUV) * smoothstep(1.0f, 0.92f, hitUV);
		confidence = edgeFactor.x * edgeFactor.y;

		// --- SOFT DEPTH-MATCHING OCCLUSION MASK ---
		if (confidence > 0.0f) {
			float sampledRawDepth = texDepth.SampleLevel(pointSampler, hitUV, 0).r;
			float3 sampledWorldPos = ReconstructWorldPos(hitUV, sampledRawDepth);

			float distToHit = length(hitWorldPos - pc.camPos.xyz);
			float distToSampled = length(sampledWorldPos - pc.camPos.xyz);

			// Calculate the absolute distance discrepancy
			float depthDiff = abs(distToHit - distToSampled);

			// Smoothly fade out the reflection if the depth mismatch is between 0.4m and 1.2m.
			// This completely eliminates the camera-pitch clipping lines.
			float depthMask = smoothstep(1.2f, 0.4f, depthDiff);
			confidence *= depthMask;
		}
		// ------------------------------------------

		return hitUV;
	}
	return float2(0.0f, 0.0f);
}
#endif

float SoftClamp(float x, float limit) {
	return limit * (1.0f - exp(-x / max(limit, 0.0001f)));
}

#ifndef __cplusplus

float4 PSMain(VSOutput input) : SV_Target0 {
	float3 litColor = texInput.SampleLevel(smp, input.uv, 0).rgb;
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	if (depth >= 1.0f)
		return float4(litColor, 1.0f);

	float4 normRough = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = normalize(normRough.xyz * 2.0f - 1.0f);
	float roughness = normRough.w;
	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float linearDepth = length(worldPos - pc.camPos.xyz);
	float ao = 1.0f;

	// --- Ambient Occlusion / GI ---
	if (pc.giMode > 0) {
		float occlusion = 0.0f;
		float3 indirectLight = float3(0.0f, 0.0f, 0.0f);
		float2 edgeFactor = smoothstep(0.0f, 0.08f, input.uv) * smoothstep(1.0f, 0.92f, input.uv);
		float screenFade = edgeFactor.x * edgeFactor.y;

		uint dw, dh;
		texDepth.GetDimensions(dw, dh);

		if (pc.giMode == 3 || pc.giMode == 4) {
			float angle = GetStableWeylNoise(uint2(input.pos.xy)) * 2.0f * 3.14159265f;
			float cosTheta = cos(angle);
			float sinTheta = sin(angle);
			int steps = max(pc.giSamples / 4, 1);
			float jitterOffset = GetRotationAngle(input.pos.xy);
			float aspect = float(dw) / max(float(dh), 1.0f);
			float focalLength = abs(pc.viewProj[1][1]);
			float uvRadius = min((pc.aoRadius * focalLength) / max(linearDepth, 0.1f), 0.2f);

			for (int d = 0; d < 4; ++d) {
				float sliceAngle = (float(d) / 4.0f) * 3.14159265f + angle;
				float2 rotatedDir = float2(cos(sliceAngle), sin(sliceAngle));
				float2 uvStep = rotatedDir * uvRadius;
				uvStep.x /= aspect;

				float max_sin_right = 0.0f;
				float max_sin_left = 0.0f;

				for (int i = 1; i <= steps; ++i) {
					float linearT = (float(i) - 0.5f + (jitterOffset - 0.5f)) / float(steps);
					float t = linearT * linearT;
					float2 uv_right = input.uv + uvStep * t;
					float2 uv_left = input.uv - uvStep * t;

					if (all(uv_right >= 0.0f) && all(uv_right <= 1.0f)) {
						float d_right = texDepth.SampleLevel(pointSampler, uv_right, 0).r;
						if (d_right < 1.0f) {
							float3 pos_right = ReconstructWorldPos(uv_right, d_right);
							float3 H = pos_right - worldPos;
							float len = length(H);
							if (len < pc.aoRadius)
								max_sin_right = max(max_sin_right, dot(H, N) / max(len, 0.001f));
						}
					}
					if (all(uv_left >= 0.0f) && all(uv_left <= 1.0f)) {
						float d_left = texDepth.SampleLevel(pointSampler, uv_left, 0).r;
						if (d_left < 1.0f) {
							float3 pos_left = ReconstructWorldPos(uv_left, d_left);
							float3 H = pos_left - worldPos;
							float len = length(H);
							if (len < pc.aoRadius)
								max_sin_left = max(max_sin_left, dot(H, N) / max(len, 0.001f));
						}
					}
				}
				occlusion += saturate((max_sin_right + max_sin_left) * 0.5f - pc.aoBias);
			}
			ao = 1.0f - SoftClamp(saturate((occlusion / 4.0f) * pc.aoPower * screenFade), 0.85f);
			litColor *= ao;
		} else {
			float angle = GetRotationAngle(input.pos.xy);
			float cosTheta = cos(angle * 2.0f * 3.14159265f);
			float sinTheta = sin(angle * 2.0f * 3.14159265f);

			for (int s = 0; s < pc.giSamples; ++s) {
				float3 sampleOffset = HemisphereSamples[s % 8];
				float3 rotatedSample =
					float3(sampleOffset.x * cosTheta - sampleOffset.y * sinTheta,
						   sampleOffset.x * sinTheta + sampleOffset.y * cosTheta, sampleOffset.z);
				float3 up = abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
				float3 tangent = normalize(cross(up, N));
				float3 bitangent = cross(N, tangent);
				float3 dir =
					rotatedSample.x * tangent + rotatedSample.y * bitangent + rotatedSample.z * N;
				float3 testPos = worldPos + dir * pc.aoRadius;

				float4 clipPos = mul(pc.viewProj, float4(testPos, 1.0f));
				float2 uv_sample = (clipPos.xy / clipPos.w) * float2(0.5f, -0.5f) + 0.5f;

				if (any(uv_sample < 0.0f) || any(uv_sample > 1.0f))
					continue;

				float sampleRawDepth = texDepth.SampleLevel(pointSampler, uv_sample, 0).r;
				if (sampleRawDepth >= 1.0f)
					continue;

				float3 sampleWorldPos = ReconstructWorldPos(uv_sample, sampleRawDepth);
				float diff =
					length(testPos - pc.camPos.xyz) - length(sampleWorldPos - pc.camPos.xyz);

				if (diff > 0.0f) {
					float finalWeight = smoothstep(0.0f, pc.aoBias * 2.0f, diff - pc.aoBias) *
										smoothstep(0.0f, 1.0f, pc.aoRadius / abs(diff));
					if (pc.giMode == 1)
						occlusion += finalWeight;
					else if (pc.giMode == 2)
						indirectLight += texInput.SampleLevel(smp, uv_sample, 0).rgb *
										 max(dot(N, dir), 0.0f) * finalWeight;
				}
			}

			if (pc.giMode == 1) {
				ao =
					1.0f -
					SoftClamp(saturate((occlusion / float(pc.giSamples)) * pc.aoPower * screenFade),
							  0.85f);
				litColor *= ao;
			} else if (pc.giMode == 2) {
				litColor += (indirectLight / float(pc.giSamples)) * pc.giIntensity * screenFade;
			}
		}
	}

	// --- Screen Space & Ray Traced Reflections ---
#ifdef DISABLE_RTR
	if (pc.enableSSR != 0 && roughness <= 0.85f) {
#else
	if ((pc.enableSSR != 0 || pc.enableRTR != 0) && roughness <= 0.85f) {
#endif
		float3 V = normalize(pc.camPos.xyz - worldPos);
		float3 R = reflect(-V, N);

		if (dot(R, N) > 0.05f) {
			float confidence = 0.0f;
			float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
			float2 hitUV = float2(0.0f, 0.0f);

			float3 biasedStartPos = worldPos + N * 0.05f;

			// 1. Attempt Screen-Space Reflection first (handles animated Pomni on-screen)
			if (pc.enableSSR != 0) {
				hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence);
			}

			// 2. If SSR fails/goes off-screen, fall back to Hardware Ray Tracing for the static
			// scene
			if (confidence < 0.1f && pc.enableRTR != 0) {
#ifdef DISABLE_RTR
				// Headless fallback
#else
				hitUV = RaytraceRTR(worldPos, N, R, confidence);
#endif
			}

			if (confidence > 0.0f) {
				confidence *= saturate(dot(R, N) * 10.0f);
				reflectionColor = texInput.SampleLevel(smp, hitUV, 0).rgb;
			}

			float iblMip = roughness * 5.0f;
			float3 iblColor = texEnvMap.SampleLevel(smp, R, iblMip).rgb;
			float3 finalReflection = lerp(iblColor, reflectionColor, confidence);

			float3 F0 = float3(0.15f, 0.15f, 0.15f);
			float3 F = F0 + (1.0f - F0) * pow(saturate(1.0f - dot(V, N)), 5.0f);

			float roughnessFade = saturate(1.0f - roughness);
			float horizonOcclusion = saturate(1.0f + dot(R, N));
			horizonOcclusion *= horizonOcclusion;

			litColor =
				lerp(litColor, litColor + finalReflection * F * roughnessFade * horizonOcclusion,
					 roughnessFade);
		} else {
			float iblMip = roughness * 5.0f;
			float3 iblColor = texEnvMap.SampleLevel(smp, R, iblMip).rgb;
			float3 F0 = float3(0.15f, 0.15f, 0.15f);
			float3 F = F0 + (1.0f - F0) * pow(saturate(1.0f - dot(V, N)), 5.0f);
			float roughnessFade = saturate(1.0f - roughness);
			litColor = lerp(litColor, litColor + iblColor * F * roughnessFade, roughnessFade);
		}
	}

	return float4(litColor, 1.0f); // Return Raw HDR
}
#endif
