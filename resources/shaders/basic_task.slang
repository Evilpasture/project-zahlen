// resources/shaders/basic_task.slang
//
// VK_EXT_mesh_shader amplification (task) stage.
//
// One workgroup screens 32 meshlets of a single instance:
//   1. bounding-sphere frustum rejection (same plane extraction as culling.slang)
//   2. meshoptimizer normal-cone back-face rejection
//   3. stream compaction of the survivors into the payload
//   4. DispatchMesh() spawns exactly one mesh workgroup per surviving meshlet
//
// Compaction uses a groupshared counter rather than wave intrinsics on purpose:
// the workgroup is 32 threads wide, but the subgroup size is 8/16/32/64
// depending on the vendor, so WavePrefixCountBits would compact per subgroup
// and produce overlapping payload slots on anything that is not exactly
// wave32. The atomic version is uniformly correct and costs one LDS op.

import common;

groupshared MeshletTaskPayload s_payload;
groupshared uint               s_visibleCount;

[shader("amplification")]
[numthreads(32, 1, 1)]
void TaskMain(uint gtid: SV_GroupIndex, uint3 gid: SV_GroupID) {
    // The indirect vertex path encodes its instance in firstInstance and pushes
    // a sentinel; mesh dispatches always push a real id, but stay defensive so
    // a stray sentinel can never index g_instances out of bounds.
    uint instId = (obj.instanceId != 4294967295u) ? obj.instanceId : 0;

    if (gtid == 0) {
        s_visibleCount        = 0;
        s_payload.instanceId  = instId;
        s_payload.baseMeshlet = gid.x * 32u;
    }
    GroupMemoryBarrierWithGroupSync();

    InstanceData inst = scene.g_instances[instId];

    uint meshletIdx = (gid.x * 32u) + gtid;
    bool isVisible  = false;

    if (inst.hasMeshlets() && meshletIdx < inst.meshletCount) {
        GPUMeshlet m = inst.fetchMeshlet(meshletIdx);

        // Skinned and morphed geometry deforms after the meshlet bounds were
        // baked, so cluster culling is disabled for it (the per-instance
        // frustum test in culling.slang still applies).
        bool boundsAreValid = !inst.isSkinned && inst.activeMorphCount == 0;

        if (!boundsAreValid) {
            isVisible = true;
        } else {
            float4x4 world      = inst.world;
            float3 sphereWorld  = mul(world, float4(m.sphereCenter, 1.0f)).xyz;
            float radiusWorld   = m.sphereRadius * MatrixMaxScale(world);

            // The shadow pass rasterises from a light, so it must cull against
            // that cascade's matrix -- and never against the normal cone,
            // whose orientation is relative to the camera.
            if (obj.isShadowPass != 0) {
                uint cascadeIndex = obj.isShadowPass - 1;
                isVisible         = SphereFrustumVisible(scene.frame.lightSpaceMatrices[cascadeIndex], sphereWorld, radiusWorld);
            } else {
                float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
                isVisible         = SphereFrustumVisible(activeVP, sphereWorld, radiusWorld);

                if (isVisible) {
                    float3 coneApexWorld = mul(world, float4(m.coneApex, 1.0f)).xyz;
                    float3 coneAxisWorld = normalize(mul((float3x3) world, m.coneAxis));
                    if (ConeBackfaceCulled(coneApexWorld, coneAxisWorld, m.coneCutoff, scene.frame.camPos.xyz)) {
                        isVisible = false;
                    }
                }
            }
        }
    }

    if (isVisible) {
        uint slot = 0;
        InterlockedAdd(s_visibleCount, 1u, slot);
        s_payload.meshletIndices[slot] = meshletIdx;
    }

    GroupMemoryBarrierWithGroupSync();

    // Only one thread may issue the dispatch, and the count must be uniform.
    DispatchMesh(s_visibleCount, 1, 1, s_payload);
}
