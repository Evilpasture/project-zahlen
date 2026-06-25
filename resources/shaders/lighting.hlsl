// resources/shaders/lighting.hlsl
#pragma pack_matrix(column_major)
#include "pbr_helpers.hlsl"
#include "uniforms.hlsl"
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
[[vk::binding(6, 0)]] Texture2D<float> shadowMap;
[[vk::binding(7, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(8, 0)]] Texture2D ltc_mat;
[[vk::binding(9, 0)]] Texture2D ltc_amp;
[[vk::binding(10, 0)]] SamplerState clampSampler;
[[vk::binding(11, 0)]] StructuredBuffer<ClusterVolume> clusterGrid;
[[vk::binding(12, 0)]] StructuredBuffer<uint> clusterIndexList;
[[vk::binding(13, 0)]] Texture2D<float4> texAmbient;
[[vk::binding(14, 0)]] SamplerState pointSampler;
[[vk::binding(15, 0)]] RaytracingAccelerationStructure tlas;

// --- Punctual Shadow Maps ---
[[vk::binding(16, 0)]] TextureCubeArray<float> punctualShadowCube; //  Omni-directional shadows
[[vk::binding(17, 0)]] Texture2DArray<float> punctualShadow2D;	   // Spotlight shadows

[[vk::constant_id(0)]] const int ENABLE_RTR = 1;

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
	float depth = texDepth.SampleLevel(pointSampler, input.uv, 0);
	if (depth >= 1.0f)
		discard;

	float4 albedoRaw = texInput.SampleLevel(smp, input.uv, 0);
	float4 normRoughRaw = texNormalRoughness.SampleLevel(smp, input.uv, 0);
	float3 N = UnpackNormalOctahedron(normRoughRaw.xy * 2.0f - 1.0f);
	float roughness = normRoughRaw.z;
	float metallic = normRoughRaw.w;

	float3 worldPos = ReconstructWorldPos(input.uv, depth, pc.invViewProj);
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

	float texelSizeWorld = frame.shadowWidth / frame.shadowResolution;
	// Add a baseline offset of 0.2 texels to prevent flat-surface acne at lower resolutions
	float normalBias = saturate(1.0 - dot(N, L_sun)) * 1.5 + 0.2;
	float3 biasedWorldPos = worldPos + N * (normalBias * texelSizeWorld);

	float4 shadowPos = mul(frame.lightSpaceMatrix, float4(biasedWorldPos, 1.0f));
	shadowPos.xy = shadowPos.xy * float2(0.5f, -0.5f) + 0.5f * shadowPos.w;

	float shadow = CalculateShadowPCSS(shadowPos, N, L_sun, input.pos.xy, shadowMap, shadowSampler,
									   pointSampler, frame.shadowResolution);

	float D = DistributionGGX(N, H_sun, roughness);
	float G_term = GeometrySmith(N, V, L_sun, roughness);
	float3 F = FresnelSchlick(saturate(dot(H_sun, V)), F0);

	float3 spec = (D * G_term * F) / max(4.0f * NdotV * NdotL_sun, 0.001f);
	float3 kD_sun = (float3(1.0f, 1.0f, 1.0f) - F) * (1.0f - metallic);

	float sunIntensity = frame.lightDir.w;
	float3 directSun =
		(kD_sun * albedoRaw.rgb / 3.14159265f + spec) * sunIntensity * NdotL_sun * shadow;

	// --- 2. Punctual and Area Lights (Clustered) ---
	float3 directPunctual = float3(0.0f, 0.0f, 0.0f);
	float viewDepth = mul(frame.unjitteredViewProj, float4(worldPos, 1.0f)).w;
	uint sliceZ = uint(max(0.0f, log(viewDepth) * frame.zScale + frame.zBias));
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

			float shadowVisibility = 1.0f;
			if (ENABLE_RTR != 0 && pc.enableRTR != 0) {
				float3 L_center = ((light.points[0].xyz + light.points[1].xyz +
									light.points[2].xyz + light.points[3].xyz) *
								   0.25f) -
								  worldPos;
				float distToCenter = length(L_center);
				float3 L_p = L_center / (distToCenter + 1e-5f);

				RayDesc ray;
				ray.Origin = worldPos + N * 0.01f;
				ray.Direction = L_p;
				ray.TMin = 0.01f;
				ray.TMax = distToCenter;

				RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
						 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
					q;
				q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
				while (q.Proceed()) {
				}

				if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
					shadowVisibility = 0.0f;
				}
			}

			directPunctual += ((1.0f - metallic) * albedoRaw.rgb * diffLTC + specLTC) *
							  light.color * light.intensity * shadowVisibility;
		} else { // POINT/SPOT LIGHT
			float3 L_unnorm = light.position - worldPos;
			float distToCenter = length(L_unnorm);
			float3 L_p = L_unnorm / (distToCenter + 1e-5f);
			float NdotL = max(dot(N, L_p), 0.0f);

			float distRatio = distToCenter / max(light.range, 0.001f);
			float window = saturate(1.0f - distRatio * distRatio);
			window *= window;
			float atten = window / (distToCenter * distToCenter + 1.0f);

			if (NdotL > 0.0f) {
				float3 H = normalize(V + L_p);
				float D_p = DistributionGGX(N, H, roughness);
				float G_p = GeometrySmith(N, V, L_p, roughness);
				float3 F_p = FresnelSchlick(max(dot(H, V), 0.0f), F0);

				float3 spec_p = (D_p * G_p * F_p) / (4.0f * max(dot(N, V), 0.0f) * NdotL + 0.0001f);
				float3 kD_p = (1.0f - F_p) * (1.0f - metallic);

				float shadowVisibility = 1.0f;
				if (ENABLE_RTR != 0 && pc.enableRTR != 0) {
					RayDesc ray;
					ray.Origin = worldPos + N * 0.01f;
					ray.Direction = L_p;
					ray.TMin = 0.01f;
					ray.TMax = distToCenter;

					RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES |
							 RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH>
						q;
					q.TraceRayInline(tlas, RAY_FLAG_NONE, 0xFF, ray);
					while (q.Proceed()) {
					}

					if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
						shadowVisibility = 0.0f;
					}
				} else if (light.shadowLayer >= 0) {
					if (light.type == 1) {	  // POINT LIGHT (Omni-directional Cubemap)
						float3 r = -L_unnorm; // Vector from light to pixel
						float d = max(abs(r.x), max(abs(r.y), abs(r.z)));
						float n = 0.1f;
						float f = light.range;

						// Reconstruct non-linear perspective depth matching Vulkan's [0.0, 1.0]
						// range [2]
						float currentDepth =
							(f / (f - n)) - ((n * f) / (f - n)) / d; // <-- FIXED FORMULA
						currentDepth -= 0.005f;						 // Bias to prevent shadow acne

						shadowVisibility = punctualShadowCube.SampleCmpLevelZero(
							shadowSampler, float4(r, light.shadowLayer), currentDepth);
					} else if (light.type == 2) { // SPOT LIGHT (2D Perspective Array)
						float4 sPos =
							mul((float4x4)light.points, float4(worldPos + N * 0.05f, 1.0f));
						sPos.xy = sPos.xy * float2(0.5f, -0.5f) + 0.5f * sPos.w;

						float depthRef = (sPos.z / sPos.w) - 0.005f;
						shadowVisibility = punctualShadow2D.SampleCmpLevelZero(
							shadowSampler, float3(sPos.xy / sPos.w, (float)light.shadowLayer),
							depthRef);
					}
				}

				directPunctual += (kD_p * albedoRaw.rgb / 3.14159265f + spec_p) * light.color *
								  light.intensity * atten * NdotL * shadowVisibility;
			}
		}
	}

	float3 litColor = ambientDiffuseAndGI * albedoRaw.rgb + directSun + directPunctual;
	return float4(litColor, ao);
}
