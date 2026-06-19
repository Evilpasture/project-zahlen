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

struct Light {
	float3 position;
	uint type; // 0=Dir, 1=Point, 2=Spot, 3=Area(Quad)
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float4 points[4]; // 4 Corners of the quad
	float radius;
	float innerConeCos;
	float outerConeCos;
	uint twoSided;
};

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
	int rtr_pad0;
	int rtr_pad1;
	int rtr_pad2;
};

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

// --- DESCRIPTOR SET LAYOUT COMPATIBILITY BINDINGS ---
[[vk::binding(0, 0)]] Texture2D<float4> texInput; // G-Buffer Target 0: Raw Unlit Albedo
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness; // G-Buffer Target 2: Normal/Rough/Metal
[[vk::binding(4, 0)]] SamplerState pointSampler;
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;

#ifndef DISABLE_RTR
[[vk::binding(6, 0)]] RaytracingAccelerationStructure tlas;
[[vk::binding(7, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(8, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(9, 0)]] Texture2D shadowMap;
[[vk::binding(10, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(11, 0)]] Texture2D ltc_mat;
[[vk::binding(12, 0)]] Texture2D ltc_amp;
[[vk::binding(13, 0)]] SamplerState clampSampler;
[[vk::binding(14, 0)]] Texture2D brdfLUT;
#else
[[vk::binding(6, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(7, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(8, 0)]] Texture2D shadowMap;
[[vk::binding(9, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(10, 0)]] Texture2D ltc_mat;
[[vk::binding(11, 0)]] Texture2D ltc_amp;
[[vk::binding(12, 0)]] SamplerState clampSampler;
[[vk::binding(13, 0)]] Texture2D brdfLUT;
#endif

// --- Octahedral Normal Unpacking ---
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

// --- Direct shadow check ---
float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;
	if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
		projCoords.z < 0.0 || projCoords.z > 1.0) {
		return 1.0;
	}
	float bias = max(0.015 * (1.0 - dot(N, L)), 0.005);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

// --- PBR analytical evaluation ---
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

float GetAverageAlbedo(float roughness) {
	return 1.0f - roughness * (0.334f - roughness * 0.125f);
}

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

// --- SUBSURFACE SCATTERING & TRANSLUCENCY HELPER ---
float3 CalculateTranslucency(float4 shadowPos, float3 N, float3 V, float3 L, float3 lightColor,
							 float distortion, float power, float scale) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;
	if (projCoords.x < 0.0f || projCoords.x > 1.0f || projCoords.y < 0.0f || projCoords.y > 1.0f ||
		projCoords.z < 0.0f || projCoords.z > 1.0f) {
		return float3(0.0f, 0.0f, 0.0f);
	}

	float shadowDepth = shadowMap.SampleLevel(smp, projCoords.xy, 0).r;
	float thickness = max(projCoords.z - shadowDepth, 0.0f);
	float thicknessScale = exp(-thickness * 45.0f);
	float3 H = normalize(L + N * distortion);
	float dotVH = saturate(dot(V, -H));

	return lightColor * pow(dotVH, power) * scale * thicknessScale;
}

// --- LTC CORE MATH ---
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

float2 RaymarchSSR(float3 worldPos, float3 startPosWS, float3 dirWS, float3 N,
				   OUT_REF(float) confidence) {
	const float maxDistance = 14.0f; // Limit trace distance for tight, realistic indoor reflections
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

		if (thickness >= 0.0f && thickness < 0.4f) {

			float t_start = (float(i) + dither) / stepCount;
			float t_end = (float(i) + 1.0f + dither) / stepCount;
			t_start = max(0.0f, t_start);

			float t_mid = 0.0f;
			float2 mid_uv = 0.0f;

			HLSL_UNROLL_N(4) for (int b = 0; b < 4; ++b) {
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
float2 RaytraceRTR(float3 worldPos, float3 N, float3 R, OUT_REF(float) confidence) {
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

float SoftClamp(float x, float limit) {
	return limit * (1.0f - exp(-x / max(limit, 0.0001f)));
}

#ifndef __cplusplus

float4 PSMain(VSOutput input) : SV_Target0 {
	float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0).r;

	if (depth >= 1.0f)
		return albedoRaw; // Skybox / Unlit

	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float2 octNormal = normRoughRaw.xy * 2.0f - 1.0f;
	float3 N = UnpackNormalOctahedron(octNormal);
	float roughness = normRoughRaw.z;
	float metallic = normRoughRaw.w;

	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float linearDepth = length(worldPos - pc.camPos.xyz);
	float ao = 1.0f;
	float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

	float2 edgeFactor = smoothstep(0.0f, 0.08f, input.uv) * smoothstep(1.0f, 0.92f, input.uv);
	float screenFade = edgeFactor.x * edgeFactor.y;

	// --- Ambient Occlusion / GI ---
	if (pc.giMode > 0) {
		if (linearDepth <= 50.0f) {
			float occlusion = 0.0f;
			uint dw, dh;
			texDepth.GetDimensions(dw, dh);

			if (pc.giMode == 3 || pc.giMode == 4) {
				float angle = GetStableWeylNoise(uint2(input.pos.xy)) * 2.0f * 3.14159265f;
				int steps = max(pc.giSamples / 6, 1);
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
									max_sin_right =
										max(max_sin_right, dot(H, N) / max(len, 0.001f));
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
				ao =
					1.0f - SoftClamp(saturate((occlusion / 4.0f) * pc.aoPower * screenFade), 0.85f);
			} else {
				float angle = GetRotationAngle(input.pos.xy);
				float cosTheta = cos(angle * 2.0f * 3.14159265f);
				float sinTheta = sin(angle * 2.0f * 3.14159265f);

				int effectiveSamples = min(pc.giSamples, 16);
				for (int s = 0; s < effectiveSamples; ++s) {
					float3 sampleOffset = HemisphereSamples[s % 8];
					float3 rotatedSample = float3(
						sampleOffset.x * cosTheta - sampleOffset.y * sinTheta,
						sampleOffset.x * sinTheta + sampleOffset.y * cosTheta, sampleOffset.z);
					float3 up =
						abs(N.y) < 0.999f ? float3(0.0f, 1.0f, 0.0f) : float3(0.0f, 0.0f, 1.0f);
					float3 tangent = normalize(cross(up, N));
					float3 bitangent = cross(N, tangent);
					float3 dir = rotatedSample.x * tangent + rotatedSample.y * bitangent +
								 rotatedSample.z * N;
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
					ao = 1.0f - SoftClamp(saturate((occlusion / float(effectiveSamples)) *
												   pc.aoPower * screenFade),
										  0.85f);
				}
			}
		}
	}

	// =========================================================================
	// --- DEFERRED ANALYTICAL LIGHTING ---
	// =========================================================================
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
	float NdotV = saturate(dot(N, V));

	float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughness), 0.0f).rg;
	float Ev_sun = envBRDF.x + envBRDF.y;
	float Eavg_sun = GetAverageAlbedo(roughness);
	float3 Favg = F0 + (1.0f - F0) / 21.0f;

	// Direct Sun Light
	float3 L_sun = normalize(frame.lightDir.xyz);
	float3 H_sun = normalize(V + L_sun + 1e-5f);
	float NdotL_sun = saturate(dot(N, L_sun));

	float4 shadowPos = mul(frame.lightSpaceMatrix, float4(worldPos, 1.0f));
	float shadow = CalculateShadow(shadowPos, N, L_sun);

	float D = DistributionGGX(N, H_sun, roughness);
	float G_term = GeometrySmith(N, V, L_sun, roughness);
	float3 F = FresnelSchlick(saturate(dot(H_sun, V)), F0);

	float3 spec = (D * G_term * F) / max(4.0f * NdotV * NdotL_sun, 0.001f);

	float3 Fms = (Favg * Favg * (1.0f - Eavg_sun)) / (1.0f - Favg * (1.0f - Eavg_sun));
	float3 spec_ms = ((1.0f - Ev_sun) * (1.0f - Ev_sun) * Fms) /
					 (3.14159265f * (1.0f - Eavg_sun) * (1.0f - Eavg_sun));
	float3 totalSpecular = spec + spec_ms;

	float3 FmsEms_sun = (Favg * (1.0f - Ev_sun)) / (1.0f - Favg * (1.0f - Ev_sun));
	float3 kD_sun = (1.0f - Ev_sun - FmsEms_sun) * (1.0f - metallic);

	float3 directSun =
		(kD_sun * albedoRaw.rgb / 3.14159265f + totalSpecular) * 10.0f * NdotL_sun * shadow;

	// Punctual and Area Lights Loop
	float3 directPunctual = float3(0.0f, 0.0f, 0.0f);
	for (uint l = 0; l < frame.lightCount; l++) {
		Light light = lights[l];
		if (light.type == 3) { // AREA LIGHT
			float2 uv = float2(roughness, sqrt(1.0f - NdotV));
			float4 t1 = ltc_mat.SampleLevel(clampSampler, uv, 0.0f);
			float3x3 Minv = float3x3(float3(t1.x, 0.0f, t1.y), float3(0.0f, 1.0f, 0.0f),
									 float3(t1.z, 0.0f, t1.w));
			float4 t2 = ltc_amp.SampleLevel(clampSampler, uv, 0.0f);
			float2 schlick = float2(t2.x, t2.y);

			float3 specLTC = LTC_Evaluate(N, V, worldPos, Minv, light.points, light.twoSided == 1);
			specLTC *= F0 * schlick.x + (1.0f - F0) * schlick.y;

			float3 diffLTC = LTC_Evaluate(N, V, worldPos, float3x3(1, 0, 0, 0, 1, 0, 0, 0, 1),
										  light.points, light.twoSided == 1);
			directPunctual += ((1.0f - metallic) * albedoRaw.rgb * diffLTC + specLTC) *
							  light.color * light.intensity;
		} else { // POINT/SPOT LIGHT
			float3 L_unnorm = light.position - worldPos;
			float distToCenter = length(L_unnorm);
			float3 L_p = L_unnorm / (distToCenter + 1e-5f);
			float NdotL = max(dot(N, L_p), 0.0f);
			float atten = 1.0f / (distToCenter * distToCenter + 0.01f);

			if (NdotL > 0.0f) {
				float3 H = normalize(V + L_p);
				float D_p = DistributionGGX(N, H, roughness);
				float G_p = GeometrySmith(N, V, L_p, roughness);
				float3 F_p = FresnelSchlick(max(dot(H, V), 0.0f), F0);

				float3 spec_p = (D_p * G_p * F_p) / (4.0f * max(dot(N, V), 0.0f) * NdotL + 0.0001f);
				float3 kD_p = (1.0f - F_p) * (1.0f - metallic);

				directPunctual += (kD_p * albedoRaw.rgb / 3.14159265f + spec_p) * light.color *
								  light.intensity * atten * NdotL;
			}
		}
	}

	// =========================================================================
	// --- INDIRECT IMAGE-BASED LIGHTING (IBL) ---
	// =========================================================================
	float3 R = reflect(-V, N);
	float3 F_rough = FresnelSchlickRoughness(max(dot(N, V), 0.0f), F0, roughness);
	float3 irradiance = EvaluateSH(N, frame.sh);

	float boxFade = 0.0f;
	if (frame.probeMin.w > 0.0f) {
		float3 boxCenter = (frame.probeMax.xyz + frame.probeMin.xyz) * 0.5f;
		float3 boxExtent = (frame.probeMax.xyz - frame.probeMin.xyz) * 0.5f;
		float3 distFromCenter = abs(worldPos - boxCenter);
		float3 normDist = distFromCenter / max(boxExtent, 0.0001f);
		float maxDist = max(max(normDist.x, normDist.y), normDist.z);
		boxFade = smoothstep(1.0f, 0.9f, maxDist);
	}

	float3 correctedR = R;
	if (boxFade > 0.0f) {
		float3 boxR = BoxParallaxCorrection(worldPos, R, frame.probeMin.xyz, frame.probeMax.xyz,
											frame.probePos.xyz);
		correctedR = lerp(R, boxR, boxFade);
	}

	float3 prefilteredColor = texEnvMap.SampleLevel(smp, correctedR, roughness * 5.0f).rgb;
	float3 FssEss = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);
	float3 specularIBL = prefilteredColor * FssEss;

	float3 FmsEms = (Favg * (1.0f - (envBRDF.x + envBRDF.y))) /
					(1.0f - Favg * (1.0f - (envBRDF.x + envBRDF.y)));
	float3 diffuseIBL = (1.0f - FssEss - FmsEms) * (1.0f - metallic) * albedoRaw.rgb * irradiance;

	float3 ambient = diffuseIBL * ao + specularIBL + (FmsEms * irradiance) * ao;

	if (pc.giMode == 2) {
		ambient += (indirectLight / float(pc.giSamples)) * pc.giIntensity * screenFade;
	}

	float3 litColor = ambient + directSun + directPunctual;

	// =========================================================================
	// --- SCREEN SPACE & RAY TRACED REFLECTIONS ---
	// =========================================================================
#ifdef DISABLE_RTR
	if (pc.enableSSR != 0 && roughness <= 0.85f) {
#else
	if ((pc.enableSSR != 0 || pc.enableRTR != 0) && roughness <= 0.85f) {
#endif
		float confidence = 0.0f;
		float3 reflectionColor = float3(0.0f, 0.0f, 0.0f);
		float2 hitUV = float2(0.0f, 0.0f);
		float3 biasedStartPos = worldPos + N * 0.05f;

		if (pc.enableSSR != 0) {
			hitUV = RaymarchSSR(worldPos, biasedStartPos, R, N, confidence);
		}

		if (confidence < 0.1f && pc.enableRTR != 0) {
#ifndef DISABLE_RTR
			hitUV = RaytraceRTR(worldPos, N, R, confidence);
#endif
		}

		if (confidence > 0.0f) {
			confidence *= saturate(dot(R, N) * 10.0f);
			reflectionColor = texInput.SampleLevel(smp, hitUV, 0).rgb;
		}

		float3 localReflection = lerp(specularIBL, reflectionColor * FssEss, confidence);

		float3 F_refl = lerp(float3(0.15f, 0.15f, 0.15f), litColor.rgb, metallic);
		float3 F_term = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
		float roughnessFade = saturate(1.0f - roughness);
		float horizonOcclusion = saturate(1.0f + dot(R, N));
		horizonOcclusion *= horizonOcclusion;

		litColor =
			lerp(litColor, litColor + localReflection * F_term * roughnessFade * horizonOcclusion,
				 roughnessFade);
	} else {
		float iblMip = roughness * 5.0f;
		float3 iblColor = texEnvMap.SampleLevel(smp, R, iblMip).rgb;
		float3 F_refl = float3(0.15f, 0.15f, 0.15f);
		float3 F_term = F_refl + (1.0f - F_refl) * pow(saturate(1.0f - dot(V, N)), 5.0f);
		float roughnessFade = saturate(1.0f - roughness);
		litColor = lerp(litColor, litColor + iblColor * F_term * roughnessFade, roughnessFade);
	}
	return float4(litColor, 1.0f); // Return Raw HDR
}
#endif
