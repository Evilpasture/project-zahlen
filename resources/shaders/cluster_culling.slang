
import cluster_grid;
import cluster_math;
import uniforms;

// Host reflects IDs 1000-1002 as this kernel's logical dispatch domain.
namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Cluster.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Cluster.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Cluster.Depth;
}

StructuredBuffer<ClusterBounds> in_Bounds;
RWStructuredBuffer<ClusterVolume> out_Grid;
RWStructuredBuffer<uint> out_IndexList;
RWStructuredBuffer<uint> out_Counter;
ConstantBuffer<FrameUniforms> frame; // Bound to slot 4 matching C++ layout
StructuredBuffer<Light> lights;      // Bound to slot 5 matching C++ layout

groupshared uint s_laneCounts[Cluster.Width * Cluster.Height];
groupshared uint s_groupBase;

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

[numthreads(Cluster.Width, Cluster.Height, 1)]
void CSMain(uint3 id: SV_DispatchThreadID, uint3 gId: SV_GroupThreadID) {
    if (id.x >= Dispatch.SizeX || id.y >= Dispatch.SizeY || id.z >= Dispatch.SizeZ) {
        return;
    }

    const uint lane = gId.y * Cluster.Width + gId.x;
    uint       cIdx = ClusterIndex(id.x, id.y, id.z);
    ClusterBounds b = in_Bounds[cIdx]; // These are now static view-space bounds!

    uint count = 0;
    uint list[Cluster.MaxLights];

    for (uint i = 0; i < frame.lightCount && count < Cluster.MaxLights; ++i) {
        if (lights[i].type == 0 || lights[i].type == 4) {
            continue; // Skip global Sun / Dir lights
        }

        // Point, Spot, and Area lights are all culled in view-space using Sphere-AABB
        if (SphereAABB(lights[i].positionView.xyz, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
            list[count++] = i;
        }
    }

    // One global atomic per 16x9 cluster slice instead of one per cluster.
    s_laneCounts[lane] = count;
    GroupMemoryBarrierWithGroupSync();

    // Serial exclusive prefix scan over this 144-thread slice. The group is
    // small (16x9) and this runs once per dispatch, so a simple scan by lane 0
    // is both correct for non-power-of-two sizes and avoids extra atomics.
    if (lane == 0) {
        uint running = 0;
        for (uint i = 0; i < Cluster.Width * Cluster.Height; ++i) {
            uint c = s_laneCounts[i];
            s_laneCounts[i] = running;
            running += c;
        }
        InterlockedAdd(out_Counter[0], running, s_groupBase);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint prefix    = s_laneCounts[lane];
    const uint offset    = s_groupBase + prefix;
    uint       safeCount = (offset + count <= Cluster.LightListCapacity) ? count :
                           (offset < Cluster.LightListCapacity ? Cluster.LightListCapacity - offset : 0u);
    for (uint j = 0; j < safeCount; ++j) {
        out_IndexList[offset + j] = list[j];
    }

    out_Grid[cIdx].offset = offset;
    out_Grid[cIdx].count  = safeCount;
}
