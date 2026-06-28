// resources/shaders/procedural_bake.hlsl
#pragma pack_matrix(column_major)

// Define our constant-id mappings
[[vk::constant_id(0)]] const int BAKE_TYPE = 0; // 0 = Voronoi, 1 = Perlin, 2 = Wave/Marble

struct PushConstants {
	uint width;
	uint height;
	float param0; // Scale
	float param1; // Randomness / Detail
	float param2; // Distortion
};
[[vk::push_constant]] PushConstants pc;

[[vk::binding(0, 0)]] RWTexture2D<float4> outTexture;

// --- NOISE MATH LIBRARIES ---
float2 Hash22(float2 p) {
	float3 p3 = frac(float3(p.xyx) * float3(0.1031f, 0.1030f, 0.0973f));
	p3 += dot(p3, p3.yzx + 33.33f);
	return frac((p3.xx + p3.yz) * p3.zy);
}

float3 EvaluateVoronoi(float2 uv) {
	float2 ip = floor(uv);
	float2 fp = frac(uv);
	float2 mg, mr;
	float md = 8.0f;

	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			float2 g = float2(float(x), float(y));
			float2 o = Hash22(ip + g) * pc.param1; // param1 = randomness
			float2 r = g + o - fp;
			float d = dot(r, r);
			if (d < md) {
				md = d;
				mr = r;
				mg = g;
			}
		}
	}
	return float3(sqrt(md), Hash22(ip + mg));
}

float EvaluatePerlin(float2 uv) {
	// Standard Perlin/Simplex gradient noise math ...
	return Hash22(floor(uv)).x; // Placeholder representation
}

[numthreads(16, 16, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	if (tid.x >= pc.width || tid.y >= pc.height)
		return;

	float2 uv = float2(tid.x, tid.y) / float2(pc.width, pc.height);
	float3 color = float3(0.0f, 0.0f, 0.0f);

	// Branching is evaluated at PIPELINE COMPILATION TIME via specialization constants
	if (BAKE_TYPE == 0) {
		float3 v = EvaluateVoronoi(uv * pc.param0);
		color = lerp(float3(0.1f, 0.12f, 0.15f), float3(v.yz, 0.5f), smoothstep(0.05f, 0.12f, v.x));
	} else if (BAKE_TYPE == 1) {
		float n = EvaluatePerlin(uv * pc.param0);
		color = float3(n, n, n);
	} else if (BAKE_TYPE == 2) {
		// Wave/Marble math
		float wave = sin(uv.x * pc.param0 + EvaluatePerlin(uv * pc.param0 * 2.0f) * pc.param2);
		color = float3(wave, wave, wave) * 0.5f + 0.5f;
	}

	outTexture[tid.xy] = float4(color, 1.0f);
}
