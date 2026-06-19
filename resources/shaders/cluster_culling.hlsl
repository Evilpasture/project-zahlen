#pragma pack_matrix(column_major)

struct ClusterBounds {
	float4 minPoint;
	float4 maxPoint;
};

struct ClusterVolume {
	uint offset;
	uint count;
};

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
	float4 pCamPos;
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

[[vk::binding(0, 0)]] StructuredBuffer<ClusterBounds> in_Bounds;
[[vk::binding(1, 0)]] RWStructuredBuffer<ClusterVolume> out_Grid;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> out_IndexList;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> out_Counter;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame; // Bound to slot 4 matching C++ layout
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;	   // Bound to slot 5 matching C++ layout

bool SphereAABB(float3 c, float r, float3 minB, float3 maxB) {
	float sq = 0.0f;
	for (int i = 0; i < 3; ++i) {
		float v = c[i];
		if (v < minB[i])
			sq += (minB[i] - v) * (minB[i] - v);
		if (v > maxB[i])
			sq += (v - maxB[i]) * (v - maxB[i]);
	}
	return sq <= (r * r);
}

[numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	uint cIdx = tid.x + (tid.y * 16) + (tid.z * 144);
	ClusterBounds b = in_Bounds[cIdx];

	uint count = 0;
	uint list[64];

	for (uint i = 0; i < frame.lightCount && count < 64; ++i) {
		if (lights[i].type == 0) { // Directional lights cover the entire scene
			list[count++] = i;
		} else {
			if (SphereAABB(lights[i].position, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
				list[count++] = i;
			}
		}
	}

	uint offset = 0;
	if (count > 0) {
		// Allocate space
		InterlockedAdd(out_Counter[0], count, offset);

		// Guard: Prevent writing past the allocated capacity of the buffer
		// Size of out_IndexList is 221184 (3456 * 64)
		if (offset + count <= 221184) {
			for (uint j = 0; j < count; ++j) {
				out_IndexList[offset + j] = list[j];
			}
		} else {
			// Fallback: If we exceed capacity, clip the written count for safety
			uint safeCount = (offset < 221184) ? (221184 - offset) : 0;
			for (uint j = 0; j < safeCount; ++j) {
				out_IndexList[offset + j] = list[j];
			}
			count = safeCount; // Shrink count reported to the grid cell
		}
	}

	out_Grid[cIdx].offset = offset;
	out_Grid[cIdx].count = count;
}
