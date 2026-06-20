// resources/shaders/ambient.hlsl
#pragma pack_matrix(column_major)
#include <SharedMath.hpp>

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

// --- G-Buffer Inputs ---
[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness;
[[vk::binding(4, 0)]] SamplerState pointSampler;
[[vk::binding(5, 0)]] TextureCube<float4> texEnvMap;
[[vk::binding(6, 0)]] Texture2D brdfLUT;
[[vk::binding(7, 0)]] SamplerState clampSampler;
[[vk::binding(8, 0)]] ConstantBuffer<FrameUniforms> frame;

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

float SoftClamp(float x, float limit) {
	return limit * (1.0f - exp(-x / max(limit, 0.0001f)));
}

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
	if (depth >= 1.0f) {
		return float4(0.0f, 0.0f, 0.0f, 1.0f);
	}

	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float2 octNormal = normRoughRaw.xy * 2.0f - 1.0f;
	float3 N = UnpackNormalOctahedron(octNormal);

	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float linearDepth = length(worldPos - pc.camPos.xyz);
	float ao = 1.0f;
	float3 indirectLight = float3(0.0f, 0.0f, 0.0f);

	float2 edgeFactor = smoothstep(0.0f, 0.08f, input.uv) * smoothstep(1.0f, 0.92f, input.uv);
	float screenFade = edgeFactor.x * edgeFactor.y;

	// --- DEFINE SMOOTH FADE BOUNDARY FROM 40m TO 50m ---
	float fade = saturate((50.0f - linearDepth) / 10.0f);

	if (pc.giMode > 0 && fade > 0.0f) {
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

			float rawAo =
				1.0f - SoftClamp(saturate((occlusion / 4.0f) * pc.aoPower * screenFade), 0.85f);
			ao = lerp(1.0f, rawAo, fade); // Apply distance fade-out

		} else {
			float angle = GetRotationAngle(input.pos.xy);
			float cosTheta = cos(angle * 2.0f * 3.14159265f);
			float sinTheta = sin(angle * 2.0f * 3.14159265f);

			int effectiveSamples = min(pc.giSamples, 16);
			for (int s = 0; s < effectiveSamples; ++s) {
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
				float rawAo = 1.0f - SoftClamp(saturate((occlusion / float(effectiveSamples)) *
														pc.aoPower * screenFade),
											   0.85f);
				ao = lerp(1.0f, rawAo, fade); // Apply distance fade-out
			} else if (pc.giMode == 2) {
				float3 rawGI =
					(indirectLight / float(effectiveSamples)) * pc.giIntensity * screenFade;
				indirectLight =
					lerp(float3(0.0f, 0.0f, 0.0f), rawGI, fade); // Apply distance fade-out
			}
		}
	}

	return float4(indirectLight, ao);
}
