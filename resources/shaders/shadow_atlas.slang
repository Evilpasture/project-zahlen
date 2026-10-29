// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

import gpu_push_constants;
import common;

// One shadow-atlas tile, one draw.
//
// The CPU has already composed this tile's world-to-clip matrix -- the light's
// view, its projection, and nothing else: the tile's place in the atlas comes
// from the viewport and scissor the pass sets, not from the transform. That
// keeps this stage a single matrix multiply and, more importantly, keeps the
// tile's depth range exactly what the lighting pass reconstructs from
// `ShadowTile.viewProj`.
//
// Unlike the cascade pass this is not multiview. A point light is six tiles in
// a 2D depth atlas rather than six layers of a cube array, so one face per draw
// and a hardware depth attachment per draw -- Early-Z and depth compression
// both stay on, which the cube-array path could only do by giving up on more
// than four lights.
[vk::push_constant]
ShadowTilePush pc;

[shader("vertex")]
ShadowGeometryOutput VSMain(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID) {
    InstanceData inst = scene.g_instances[instanceId];
    float4 worldPos, activeVPPos;
    SceneGeometryVertex p = inst.EvaluateVertex<false>(inst.fetchRasterIndex(vertexId), worldPos);
    p.instanceId = instanceId;
    p.pos        = mul(pc.viewProj, worldPos);
    return PackShadowOutput(p);
}
