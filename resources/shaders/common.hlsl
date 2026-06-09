// resources/shaders/common.hlsl
#pragma pack_matrix(column_major)

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
	float3 padding;
	float4 sh[9];
	float4 probeMin; // XYZ: boxMin, W: useLocalProbe (0.0 or 1.0)
	float4 probeMax; // XYZ: boxMax, W: unused
	float4 probePos; // XYZ: probePos, W: unused
};

struct ObjectConstants {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint isShadowPass;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;
	uint vertexCount;
	uint morphOffset;
	uint activeMorphCount;
	uint indexCount; // Added
	uint pad;		 // Added
	float4 morphWeights;
	float4 baseColorFactor;
};

struct InstanceData {
	float4x4 world;
	float4x4 prevWorld;
	uint albedoIdx;
	uint normalIdx;
	uint pbrIdx;
	uint emissiveIdx;
	uint vertexCount;
	float cullRadius;
	float metallicFactor;
	float roughnessFactor;
	float alphaCutoff;
	uint alphaMode;
	uint jointOffset;
	uint isSkinned;
	uint morphOffset;
	uint activeMorphCount;
	uint indexCount; // Added
	uint pad;		 // Added
	float4 morphWeights;
	float4 baseColorFactor;
};
struct GPUJoint {
	float4 col0;
	float4 col1;
	float4 col2;
	float4 col3;
};

#ifndef SKIP_BINDINGS
[[vk::push_constant]] ObjectConstants obj;

[[vk::binding(0, 0)]] Texture2D globalTextures[];
[[vk::binding(6, 0)]] StructuredBuffer<InstanceData> g_instances;
[[vk::binding(1, 0)]] SamplerState defaultSampler;
[[vk::binding(2, 0)]] Texture2D shadowMap;
[[vk::binding(3, 0)]] SamplerComparisonState shadowSampler;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame;
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;

[[vk::binding(7, 0)]] StructuredBuffer<GPUJoint> g_joints;

[[vk::binding(8, 0)]] TextureCube prefilteredMap; // Shifted
[[vk::binding(9, 0)]] Texture2D brdfLUT;		  // Shifted
[[vk::binding(10, 0)]] StructuredBuffer<float4> g_morphDeltas;
[[vk::binding(11, 0)]] SamplerState clampSampler;
[[vk::binding(12, 0)]] Texture2D ltc_mat;
[[vk::binding(13, 0)]] Texture2D ltc_amp;

struct VSInput {
	[[vk::location(0)]] float3 position : POSITION;
	[[vk::location(1)]] float3 normal : NORMAL;
	[[vk::location(2)]] float4 tangent : TANGENT;
	[[vk::location(3)]] float2 uv : TEXCOORD;
	[[vk::location(4)]] float4 color : COLOR;
	[[vk::location(5)]] uint4 joints : JOINTS;
	[[vk::location(6)]] float4 weights : WEIGHTS;
};

struct VSOutput {
	float4 pos : SV_Position;
	float4 currClip : TEXCOORD0;
	float4 prevClip : TEXCOORD1;
	float3 worldPos : POSITION;
	float3 normal : NORMAL;
	float4 tangent : TANGENT;
	float2 uv : TEXCOORD2;
	float4 shadowPos : TEXCOORD3;
	float4 color : COLOR;
	nointerpolation uint4 materialIndices : TEXCOORD4;
	nointerpolation float4 baseColorFactor : TEXCOORD5;
	nointerpolation float3 pbrFactors : TEXCOORD6;
	nointerpolation uint alphaMode : TEXCOORD7;
};

struct PSOutput {
	float4 color : SV_Target0;
	float2 velocity : SV_Target1;
	float4 normalRoughness : SV_Target2;
};

float3 EvaluateSH(float3 N, float4 sh[9]) {
	float3 result =
		sh[0].xyz * 0.282095f + sh[1].xyz * -0.488603f * N.y + sh[2].xyz * 0.488603f * N.z +
		sh[3].xyz * -0.488603f * N.x + sh[4].xyz * 1.092548f * N.x * N.y +
		sh[5].xyz * -1.092548f * N.y * N.z + sh[6].xyz * 0.315392f * (3.0f * N.z * N.z - 1.0f) +
		sh[7].xyz * -1.092548f * N.x * N.z + sh[8].xyz * 0.546274f * (N.x * N.x - N.y * N.y);
	return max(result, float3(0.0f, 0.0f, 0.0f));
}

// --- SKELETAL SKINNING ---
float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z +
				  j0.col3 * position.w) *
					 weights.x +
				 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z +
				  j1.col3 * position.w) *
					 weights.y +
				 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z +
				  j2.col3 * position.w) *
					 weights.z +
				 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z +
				  j3.col3 * position.w) *
					 weights.w;
	return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset) {
	// If the sum of weights is zero, return the original direction to avoid NaN
	if (dot(weights, float4(1.5, 1.0, 1.0, 1.0)) < 0.001) {
		return direction;
	}

	GPUJoint j0 = g_joints[jointOffset + joints.x];
	GPUJoint j1 = g_joints[jointOffset + joints.y];
	GPUJoint j2 = g_joints[jointOffset + joints.z];
	GPUJoint j3 = g_joints[jointOffset + joints.w];

	float3 dir =
		(j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) *
			weights.x +
		(j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) *
			weights.y +
		(j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) *
			weights.z +
		(j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) *
			weights.w;

	return dir;
}

// --- SHADOW CALCULATOR ---
float CalculateShadow(float4 shadowPos, float3 N, float3 L) {
	float3 projCoords = shadowPos.xyz / shadowPos.w;

	if (projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0 ||
		projCoords.z < 0.0 || projCoords.z > 1.0) {
		return 1.0;
	}

	float bias = max(0.015 * (1.0 - dot(N, L)), 0.005);
	return shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy, projCoords.z - bias).r;
}

// Specular Fresnel roughness helper
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
	return F0 + (max(float3(1.0 - roughness, 1.0 - roughness, 1.0 - roughness), F0) - F0) *
					pow(saturate(1.0 - cosTheta), 5.0);
}

float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset,
							uint activeMorphCount, float4 weights) {
	float3 displacement = float3(0, 0, 0);

	for (uint i = 0; i < activeMorphCount; ++i) {
		// Calculate index inside the contiguous GPU buffer
		uint deltaIndex = morphOffset + (i * vertexCount) + vertexId;
		displacement += g_morphDeltas[deltaIndex].xyz * weights[i];
	}

	return displacement;
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
	// 1. Construct Orthonormal Basis
	float3 T1, T2;
	T1 = normalize(V - N * dot(V, N));
	T2 = cross(N, T1);
	float3x3 R = float3x3(T1, T2, N);

	// 2. Transform polygon to local tangent space and apply LTC inverse matrix
	float3 L[5]; // Max 5 vertices after clipping
	L[0] = mul(Minv, mul(R, points[0].xyz - P));
	L[1] = mul(Minv, mul(R, points[1].xyz - P));
	L[2] = mul(Minv, mul(R, points[2].xyz - P));
	L[3] = mul(Minv, mul(R, points[3].xyz - P));

	// 3. Sutherland-Hodgman Clipping against the horizon (z = 0)
	int n = 0;
	float3 clipped[5];

	// Unrolled fast clipper
	[unroll] for (int i = 0; i < 4; ++i) {
		float3 v1 = L[i];
		float3 v2 = L[(i + 1) % 4];

		if (v1.z > 0.0) {
			clipped[n++] = v1;
		}
		// If the edge crosses the horizon plane
		if ((v1.z > 0.0 && v2.z < 0.0) || (v1.z < 0.0 && v2.z > 0.0)) {
			float t = v1.z / (v1.z - v2.z);
			clipped[n++] = lerp(v1, v2, t);
		}
	}

	if (n < 3)
		return float3(0, 0, 0); // Completely below horizon
	[unroll] for (int j = 0; j < n; ++j) {
		clipped[j] = normalize(clipped[j]);
	}
	// 4. Integrate Area
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

// --- Kulla-Conty Energy Compensation Helpers ---
float GetDirectionalAlbedo(float NoX, float roughness) {
	// Reuses your existing 2D BRDF LUT to evaluate directional albedo on-the-fly
	float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(saturate(NoX), roughness), 0.0f).rg;
	return envBRDF.x + envBRDF.y;
}

float GetAverageAlbedo(float roughness) {
	// Highly accurate polynomial fit for GGX average hemispherical albedo (Fdez-Agüera)
	return 1.0f - roughness * (0.334f - roughness * 0.125f);
}

float3 EvaluateKullaContyDirect(float NoV, float NoL, float roughness, float3 F0, float3 Favg) {
	float Ev = GetDirectionalAlbedo(NoV, roughness);
	float El = GetDirectionalAlbedo(NoL, roughness);
	float Eavg = GetAverageAlbedo(roughness);

	float Ems_v = 1.0f - Ev;
	float Ems_l = 1.0f - El;
	float Ems_avg = 1.0f - Eavg;

	// Precalculate the average multi-scattering specular reflectance
	float3 Fms = (Favg * Favg * Ems_avg) / (1.0f - Favg * Ems_avg);

	// Isotropic, diffuse-like specular compensation lobe
	float3 f_add = (Ems_v * Ems_l * Fms) / (3.14159265f * Ems_avg * Ems_avg);

	return f_add;
}

float3 BoxParallaxCorrection(float3 posWS, float3 R, float3 boxMin, float3 boxMax,
							 float3 probePos) {
	// Prevent division-by-zero by clamping the reflection vector with a tiny offset
	float3 invR = 1.0f / max(abs(R), 0.00001f) * sign(R);

	// Calculate the intersection distances along each coordinate axis
	float3 t1 = (boxMax - posWS) * invR;
	float3 t2 = (boxMin - posWS) * invR;
	float3 tMax = max(t1, t2); // Furthest intersections along the ray

	// Find the closest exit plane of the box
	float distance = min(min(tMax.x, tMax.y), tMax.z);

	// Calculate the 3D world-space intersection point
	float3 intersectPositionWS = posWS + R * distance;

	// Correct the reflection vector to look from the perspective of the probe capture point
	return normalize(intersectPositionWS - probePos);
}
#endif // SKIP_BINDINGS
