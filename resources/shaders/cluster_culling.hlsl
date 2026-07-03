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

[numthreads(16, 9, 1)] void CSMain(uint3 id : SV_DispatchThreadID) {
	uint cIdx = id.x + (id.y * 16) + (id.z * 144);
	ClusterBounds b = in_Bounds[cIdx]; // These are now static view-space bounds!

	uint count = 0;
	uint list[64];

	for (uint i = 0; i < frame.lightCount && count < 64; ++i) {
		if (lights[i].type == 0 || lights[i].type == 4) {
			continue; // Skip global Sun / Dir lights
		}

		// Point, Spot, and Area lights are all culled in view-space using Sphere-AABB
		if (SphereAABB(lights[i].positionView, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
			list[count++] = i;
		}
	}

	uint offset = 0;
	if (count > 0) {
		InterlockedAdd(out_Counter[0], count, offset);

		if (offset + count <= 221184) {
			for (uint j = 0; j < count; ++j) {
				out_IndexList[offset + j] = list[j];
			}
		} else {
			uint safeCount = (offset < 221184) ? (221184 - offset) : 0;
			for (uint j = 0; j < safeCount; ++j) {
				out_IndexList[offset + j] = list[j];
			}
			count = safeCount;
		}
	}

	out_Grid[cIdx].offset = offset;
	out_Grid[cIdx].count = count;
}
