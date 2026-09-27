
import common;

groupshared MeshletTaskPayload s_payload;
groupshared uint               s_visibleCount;

[shader("amplification")]
[numthreads(32, 1, 1)]
void TaskMain(uint gtid: SV_GroupIndex, uint3 gid: SV_GroupID) {
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

        bool boundsAreValid = !inst.isSkinned && inst.activeMorphCount == 0;

        if (!boundsAreValid) {
            isVisible = true;
        } else {
            float4x4 world      = inst.world;
            float3 sphereWorld  = mul(world, float4(m.sphereCenter, 1.0f)).xyz;
            float radiusWorld   = m.sphereRadius * MatrixMaxScale(world);

            if (obj.isShadowPass != 0) {
                isVisible = false;
                for (uint c = 0; c < 4; ++c) {
                    if (SphereFrustumVisible(scene.frame.lightSpaceMatrices[c], sphereWorld, radiusWorld)) {
                        isVisible = true;
                        break;
                    }
                }
            } else {
                float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
                isVisible         = SphereFrustumVisible(activeVP, sphereWorld, radiusWorld);

                if (isVisible) {
                    float3 coneAxisWorld = normalize(mul((float3x3) world, m.coneAxis));
                    if (ConeBackfaceCulledSphere(sphereWorld, radiusWorld, coneAxisWorld, m.coneCutoff, scene.frame.camPos.xyz)) {
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

    DispatchMesh(s_visibleCount, 1, 1, s_payload);
}
