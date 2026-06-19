// resources/shaders/lighting.hlsl
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

struct Light {
	float3 position;
	uint type; // 0=Dir, 1=Point, 2=Spot, 3=Area(Quad)
	float3 color;
	float intensity;
	float3 direction;
	float range;
	float4 points[4];
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
	float zScale;
	float zBias;
	int rtr_pad0;
};

struct ClusterVolume {
	uint offset;
	uint count;
};

// --- G-Buffer & Lighting Inputs ---
[[vk::binding(0, 0)]] Texture2D<float4> texInput;
[[vk::binding(1, 0)]] SamplerState smp;
[[vk::binding(2, 0)]] Texture2D<float> texDepth;
[[vk::binding(3, 0)]] Texture2D<float4> texNormalRoughness;
[[vk::binding(4, 0)]] StructuredBuffer<Light> lights;
[[vk::binding(5, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(6, 0)]] Texture2D shadowMap;
[[vk::binding(7, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(8, 0)]] Texture2D ltc_mat;
[[vk::binding(9, 0)]] Texture2D ltc_amp;
[[vk::binding(10, 0)]] SamplerState clampSampler;
[[vk::binding(11, 0)]] StructuredBuffer<ClusterVolume> clusterGrid;
[[vk::binding(12, 0)]] StructuredBuffer<uint> clusterIndexList;
[[vk::binding(13, 0)]] Texture2D<float4> texAmbient; // Ambient input from Pass 1

float3 UnpackNormalOctahedron(float2 H_unpacked) {
	float3 N = float3(H_unpacked, 1.0 - abs(H_unpacked.x) - abs(H_unpacked.y));
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

float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;
	if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
		projCoords.z < 0.0 || projCoords.z > 1.0) {
		return 0.0; // <-- Change from 1.0 to 0.0 to prevent sun leakage indoors
	}
	float bias = max(0.015 * (1.0 - dot(N, L)), 0.005);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

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
	float depth =
		texDepth.SampleLevel(smp, input.uv, 0).r; // <-- FIXED: CHANGED pointSampler TO smp
	if (depth >= 1.0f)
		discard;

	float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
	float roughness = normRoughRaw.z;
	float metallic = normRoughRaw.w;

	float3 worldPos = ReconstructWorldPos(input.uv, depth);
	float3 V = normalize(pc.camPos.xyz - worldPos);
	float3 F0 = lerp(float3(0.04f, 0.04f, 0.04f), albedoRaw.rgb, metallic);
	float NdotV = saturate(dot(N, V));

	float4 ambientData = texAmbient.SampleLevel(smp, input.uv, 0);
	float3 ambientDiffuseAndGI = ambientData.rgb;
	float ao = ambientData.a;

	// --- 1. Direct Sun Light ---
	float3 L_sun = normalize(frame.lightDir.xyz);
	float3 H_sun = normalize(V + L_sun + 1e-5f);
	float NdotL_sun = saturate(dot(N, L_sun));

	float4 shadowPos = mul(frame.lightSpaceMatrix, float4(worldPos, 1.0f));
	float shadow = CalculateShadow(shadowPos, N, L_sun);

	float D = DistributionGGX(N, H_sun, roughness);
	float G_term = GeometrySmith(N, V, L_sun, roughness);
	float3 F = FresnelSchlick(saturate(dot(H_sun, V)), F0);

	float3 spec = (D * G_term * F) / max(4.0f * NdotV * NdotL_sun, 0.001f);
	float3 kD_sun = (1.0f - metallic);

	float3 directSun = (kD_sun * albedoRaw.rgb / 3.14159265f + spec) * 10.0f * NdotL_sun * shadow;

	// --- 2. Punctual and Area Lights (Clustered) ---
	float3 directPunctual = float3(0.0f, 0.0f, 0.0f);
	float linearDepth = length(worldPos - pc.camPos.xyz);
	uint sliceZ = uint(max(0.0f, log(linearDepth) * frame.zScale + frame.zBias));
	uint cIdx = min(uint(input.uv.x * 16.0f), 15u) + (min(uint(input.uv.y * 9.0f), 8u) * 16) +
				(min(sliceZ, 23u) * 144);

	ClusterVolume cluster = clusterGrid[cIdx];

	for (uint i = 0; i < cluster.count; ++i) {
		uint l = clusterIndexList[cluster.offset + i];
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

	float3 litColor = ambientDiffuseAndGI * albedoRaw.rgb + directSun + directPunctual;
	return float4(litColor, ao);
}
