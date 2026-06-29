#pragma pack_matrix(column_major)
#include "uniforms.hlsl"
struct ClusterBounds {
	float4 minPoint;
	float4 maxPoint;
};

struct ClusterVolume {
	uint offset;
	uint count;
};

[[vk::binding(0, 0)]] StructuredBuffer<ClusterBounds> in_Bounds;
[[vk::binding(1, 0)]] RWStructuredBuffer<ClusterVolume> out_Grid;
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> out_IndexList;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> out_Counter;
[[vk::binding(4, 0)]] ConstantBuffer<FrameUniforms> frame; // Bound to slot 4 matching C++ layout
[[vk::binding(5, 0)]] StructuredBuffer<Light> lights;	   // Bound to slot 5 matching C++ layout

// --- Sphere vs Axis-Aligned Bounding Box (AABB) ---
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

// --- Cone vs Plane (Consistently evaluates if the entire cone lies behind the plane) ---
bool ConeBehindPlane(float3 T, float3 D, float R, float r, float3 N, float d) {
	// Value of the plane equation at the cone tip
	float valTip = dot(N, T) + d;

	// Project the plane normal onto the base circle's plane
	float sinTheta = sqrt(max(0.0f, 1.0f - dot(N, D) * dot(N, D)));

	// FIXED: Changed minus to plus to correctly evaluate the furthest point on the base circle
	float valBase = dot(N, T + D * R) + d + r * sinTheta;

	// The cone is completely behind the plane if both points are in the negative halfspace
	return (valTip < 0.0f) && (valBase < 0.0f);
}

[numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
	uint cIdx = tid.x + (tid.y * 16) + (tid.z * 144);
	ClusterBounds b = in_Bounds[cIdx];

	uint count = 0;
	uint list[64];

	for (uint i = 0; i < frame.lightCount && count < 64; ++i) {
		if (lights[i].type == 0 || lights[i].type == 4) {
			// Skip completely. Directional and Sun lights are evaluated globally,
			// saving critical cluster slots for local lights.
			continue;
		} else if (lights[i].type == 1 || lights[i].type == 3) { // Point and Area Lights
			if (SphereAABB(lights[i].position, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
				list[count++] = i;
			}
		} else if (lights[i].type == 2) { // Spotlights (Cone culling)
			// Phase 1: Quick Sphere-AABB check to prune far lights
			if (SphereAABB(lights[i].position, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {

				float3 T = lights[i].position;
				float3 dir = lights[i].direction;
				float R = lights[i].range;

				// Calculate cone base radius using spot angle properties
				float cosTheta = lights[i].outerConeCos;
				float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
				float r = R * (sinTheta / max(0.0001f, cosTheta));

				// Phase 2: Cull if the spotlight cone lies entirely behind any of the 6 box planes
				bool culled = ConeBehindPlane(T, dir, R, r, float3(1, 0, 0), -b.minPoint.x) ||
							  ConeBehindPlane(T, dir, R, r, float3(-1, 0, 0), b.maxPoint.x) ||
							  ConeBehindPlane(T, dir, R, r, float3(0, 1, 0), -b.minPoint.y) ||
							  ConeBehindPlane(T, dir, R, r, float3(0, -1, 0), b.maxPoint.y) ||
							  ConeBehindPlane(T, dir, R, r, float3(0, 0, 1), -b.minPoint.z) ||
							  ConeBehindPlane(T, dir, R, r, float3(0, 0, -1), b.maxPoint.z);

				if (!culled) {
					list[count++] = i;
				}
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
