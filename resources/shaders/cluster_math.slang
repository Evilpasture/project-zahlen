// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative clustered-lighting slice math. Lighting, volumetric inject,
// and cluster-bounds generation all import this module so the logarithmic
// depth mapping cannot drift across shaders.
module cluster_math;

import cluster_grid;

// Slice near/far are specialization constants so a pipeline can override the
// defaults without rewriting this module. Shaders evaluate the log mapping
// in-place from these values.
namespace Cluster {
    [[vk::constant_id(1100)]] public const float Near = 0.1f;
    [[vk::constant_id(1101)]] public const float Far  = 1000.0f;
}

public struct ClusterBounds {
    public float4 minPoint;
    public float4 maxPoint;
};

public struct ClusterVolume {
    public uint offset;
    public uint count;
};

[ForceInline]
public float ClusterZScale() {
    return float(Cluster.Depth) / log(Cluster.Far / Cluster.Near);
}

[ForceInline]
public float ClusterZBias() {
    return -(float(Cluster.Depth) * log(Cluster.Near)) / log(Cluster.Far / Cluster.Near);
}

[ForceInline]
public float ClusterSliceNear(uint z) {
    return Cluster.Near * pow(Cluster.Far / Cluster.Near, float(z) / float(Cluster.Depth));
}

[ForceInline]
public float ClusterSliceFar(uint z) {
    return ClusterSliceNear(z + 1);
}

[ForceInline]
public uint ViewDepthToClusterZ(float viewDepth) {
    return uint(max(0.0f, log(viewDepth) * ClusterZScale() + ClusterZBias()));
}

[ForceInline]
public uint ClusterIndex(uint x, uint y, uint z) {
    uint cx = min(x, Cluster.Width - 1);
    uint cy = min(y, Cluster.Height - 1);
    uint cz = min(z, Cluster.Depth - 1);
    return cx + (cy * Cluster.Width) + (cz * Cluster.Width * Cluster.Height);
}

[ForceInline]
public uint ClusterIndexFromUV(float2 uv, float viewDepth) {
    uint x = uint(uv.x * float(Cluster.Width));
    uint y = uint(uv.y * float(Cluster.Height));
    return ClusterIndex(x, y, ViewDepthToClusterZ(viewDepth));
}
