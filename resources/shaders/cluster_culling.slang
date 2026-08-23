
import uniforms;

static const uint CLUSTER_GRID_WIDTH  = 16;
static const uint CLUSTER_GRID_HEIGHT = 9;
static const uint CLUSTER_GRID_DEPTH  = 24;

// Reserved metadata IDs consumed by ReflectComputeDispatchSize. They make this
// shader the authority for its fixed logical dispatch domain as well as its
// local [numthreads] dimensions.
[[vk::constant_id(1000)]] const uint ZHLN_DISPATCH_SIZE_X = CLUSTER_GRID_WIDTH;
[[vk::constant_id(1001)]] const uint ZHLN_DISPATCH_SIZE_Y = CLUSTER_GRID_HEIGHT;
[[vk::constant_id(1002)]] const uint ZHLN_DISPATCH_SIZE_Z = CLUSTER_GRID_DEPTH;

struct ClusterBounds {
    float4 minPoint;
    float4 maxPoint;
};

struct ClusterVolume {
    uint offset;
    uint count;
};

StructuredBuffer<ClusterBounds> in_Bounds;
RWStructuredBuffer<ClusterVolume> out_Grid;
RWStructuredBuffer<uint> out_IndexList;
RWStructuredBuffer<uint> out_Counter;
ConstantBuffer<FrameUniforms> frame; // Bound to slot 4 matching C++ layout
StructuredBuffer<Light> lights;      // Bound to slot 5 matching C++ layout

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

[numthreads(CLUSTER_GRID_WIDTH, CLUSTER_GRID_HEIGHT, 1)]
void CSMain(uint3 id: SV_DispatchThreadID) {
    // Defense-in-depth: the host reflects the logical domain above and derives
    // workgroup counts from [numthreads]. Still reject every thread outside the
    // domain before it can index in_Bounds/out_Grid or update the counter.
    if (id.x >= ZHLN_DISPATCH_SIZE_X || id.y >= ZHLN_DISPATCH_SIZE_Y || id.z >= ZHLN_DISPATCH_SIZE_Z) {
        return;
    }

    uint cIdx       = id.x + (id.y * ZHLN_DISPATCH_SIZE_X) + (id.z * ZHLN_DISPATCH_SIZE_X * ZHLN_DISPATCH_SIZE_Y);
    ClusterBounds b = in_Bounds[cIdx]; // These are now static view-space bounds!

    uint count = 0;
    uint list[64];

    for (uint i = 0; i < frame.lightCount && count < 64; ++i) {
        if (lights[i].type == 0 || lights[i].type == 4) {
            continue; // Skip global Sun / Dir lights
        }

        // Point, Spot, and Area lights are all culled in view-space using Sphere-AABB
        if (SphereAABB(lights[i].positionView.xyz, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
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
    out_Grid[cIdx].count  = count;
}
