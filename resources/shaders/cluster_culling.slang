
import cluster_grid;
import cluster_math;
import uniforms;

namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Cluster.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Cluster.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Cluster.Depth;
}

StructuredBuffer<ClusterBounds> in_Bounds;
RWStructuredBuffer<ClusterVolume> out_Grid;
RWStructuredBuffer<uint> out_IndexList;
RWStructuredBuffer<uint> out_Counter;
ConstantBuffer<FrameUniforms> frame;
StructuredBuffer<Light> lights;

groupshared uint s_waveSums[Cluster.Width * Cluster.Height];
groupshared uint s_waveCount;
groupshared uint s_groupBase;

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

bool ConeBehindPlane(float3 T, float3 D, float R, float r, float3 N, float d) {
    float valTip = dot(N, T) + d;

    float sinTheta = sqrt(max(0.0f, 1.0f - dot(N, D) * dot(N, D)));

    float valBase = dot(N, T + D * R) + d + r * sinTheta;

    return (valTip < 0.0f) && (valBase < 0.0f);
}

[numthreads(Cluster.Width, Cluster.Height, 1)]
void CSMain(uint3 id: SV_DispatchThreadID, uint3 gId: SV_GroupThreadID) {
    if (id.x >= Dispatch.SizeX || id.y >= Dispatch.SizeY || id.z >= Dispatch.SizeZ) {
        return;
    }

    const uint lane = gId.y * Cluster.Width + gId.x;
    uint       cIdx = ClusterIndex(id.x, id.y, id.z);
    ClusterBounds b = in_Bounds[cIdx];

    uint count = 0;
    uint list[Cluster.MaxLights];

    for (uint i = 0; i < frame.lightCount && count < Cluster.MaxLights; ++i) {
        if (lights[i].type == 0 || lights[i].type == 4) {
            continue;
        }

        if (SphereAABB(lights[i].positionView.xyz, lights[i].range, b.minPoint.xyz, b.maxPoint.xyz)) {
            list[count++] = i;
        }
    }


    if (lane == 0) {
        s_waveCount = 0;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint wavePrefix = WavePrefixSum(count);
    const uint waveSum    = WaveActiveSum(count);

    uint slot = 0;
    if (WaveGetLaneIndex() == 0) {
        InterlockedAdd(s_waveCount, 1u, slot);
        s_waveSums[slot] = waveSum;
    }
    slot = WaveReadLaneAt(slot, 0);
    GroupMemoryBarrierWithGroupSync();

    if (lane == 0) {
        uint running = 0;
        for (uint i = 0; i < s_waveCount; ++i) {
            uint c = s_waveSums[i];
            s_waveSums[i] = running;
            running += c;
        }
        InterlockedAdd(out_Counter[0], running, s_groupBase);
    }
    GroupMemoryBarrierWithGroupSync();

    const uint prefix    = s_waveSums[slot] + wavePrefix;
    const uint offset    = s_groupBase + prefix;
    uint       safeCount = (offset + count <= Cluster.LightListCapacity) ? count :
                           (offset < Cluster.LightListCapacity ? Cluster.LightListCapacity - offset : 0u);
    for (uint j = 0; j < safeCount; ++j) {
        out_IndexList[offset + j] = list[j];
    }

    out_Grid[cIdx].offset = offset;
    out_Grid[cIdx].count  = safeCount;
}
