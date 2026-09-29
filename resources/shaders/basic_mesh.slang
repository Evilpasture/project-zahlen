

import common;

#ifndef ZHLN_MESHLET_MAX_VERTICES
#define ZHLN_MESHLET_MAX_VERTICES 64
#endif

#ifndef ZHLN_MESHLET_MAX_TRIANGLES
#define ZHLN_MESHLET_MAX_TRIANGLES 124
#endif

E.Output EmitMeshletVertex<E : IScenePassOutput, bool isShadow>(
    GPUMeshlet m, InstanceData inst, uint instanceId, uint gtid, uint viewId
) {
    uint actualVertexId = inst.fetchMeshletVertex(m.vertexOffset, gtid);
    float4 worldPos;
    SceneGeometryVertex output = inst.EvaluateVertex<!isShadow>(actualVertexId, worldPos);
    output.instanceId = instanceId;

    if (isShadow) {
        output.pos = mul(scene.frame.lightSpaceMatrices[min(viewId, 3u)], worldPos);
        // Shadow draws previously sent only the albedo index to the pixel shader.
        output.materialIndices = uint4(output.materialIndices.x, 0u, 0u, 0u);
    } else {
        float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
        output.pos = mul(activeVP, worldPos);
    }

    return E.Emit(output, viewId);
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MeshMain(
    uint gtid: SV_GroupIndex,
    uint3 gid: SV_GroupID,
    in payload MeshletTaskPayload payload,
    out vertices GBufferGeometryOutput outVertices[ZHLN_MESHLET_MAX_VERTICES],
    out indices uint3 outTriangles[ZHLN_MESHLET_MAX_TRIANGLES]
) {
    uint instId       = payload.instanceId;
    InstanceData inst = scene.g_instances[instId];

    if (!inst.hasMeshlets()) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    uint meshletIdx = payload.meshletIndices[gid.x];
    if (meshletIdx >= inst.meshletCount) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    GPUMeshlet m = inst.fetchMeshlet(meshletIdx);

    uint vertexCountOut = min(m.vertexCount, (uint) ZHLN_MESHLET_MAX_VERTICES);
    uint triangleCount  = min(m.triangleCount, (uint) ZHLN_MESHLET_MAX_TRIANGLES);
    SetMeshOutputCounts(vertexCountOut, triangleCount);

    if (gtid < vertexCountOut) {
        outVertices[gtid] = EmitMeshletVertex<GBufferPassOutput, false>(m, inst, instId, gtid, 0);
    }

    for (uint i = gtid; i < triangleCount; i += 64u) {
        uint3 tri       = inst.fetchMeshletTriangle(m.triangleOffset, i);
        outTriangles[i] = min(tri, uint3(vertexCountOut - 1u));
    }
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MeshMainForward(
    uint gtid: SV_GroupIndex,
    uint3 gid: SV_GroupID,
    in payload MeshletTaskPayload payload,
    out vertices ForwardGeometryOutput outVertices[ZHLN_MESHLET_MAX_VERTICES],
    out indices uint3 outTriangles[ZHLN_MESHLET_MAX_TRIANGLES]
) {
    uint instId       = payload.instanceId;
    InstanceData inst = scene.g_instances[instId];

    if (!inst.hasMeshlets()) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    uint meshletIdx = payload.meshletIndices[gid.x];
    if (meshletIdx >= inst.meshletCount) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    GPUMeshlet m = inst.fetchMeshlet(meshletIdx);

    uint vertexCountOut = min(m.vertexCount, (uint) ZHLN_MESHLET_MAX_VERTICES);
    uint triangleCount  = min(m.triangleCount, (uint) ZHLN_MESHLET_MAX_TRIANGLES);
    SetMeshOutputCounts(vertexCountOut, triangleCount);

    if (gtid < vertexCountOut) {
        outVertices[gtid] = EmitMeshletVertex<ForwardPassOutput, false>(m, inst, instId, gtid, 0);
    }

    for (uint i = gtid; i < triangleCount; i += 64u) {
        uint3 tri       = inst.fetchMeshletTriangle(m.triangleOffset, i);
        outTriangles[i] = min(tri, uint3(vertexCountOut - 1u));
    }
}

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MeshMainShadow(
    uint gtid: SV_GroupIndex,
    uint3 gid: SV_GroupID,
    uint viewId: SV_ViewID,
    in payload MeshletTaskPayload payload,
    out vertices ShadowGeometryOutput outVertices[ZHLN_MESHLET_MAX_VERTICES],
    out indices uint3 outTriangles[ZHLN_MESHLET_MAX_TRIANGLES]
) {
    uint instId       = payload.instanceId;
    InstanceData inst = scene.g_instances[instId];

    if (!inst.hasMeshlets()) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    uint meshletIdx = payload.meshletIndices[gid.x];
    if (meshletIdx >= inst.meshletCount) {
        SetMeshOutputCounts(0, 0);
        return;
    }

    GPUMeshlet m = inst.fetchMeshlet(meshletIdx);

    uint vertexCountOut = min(m.vertexCount, (uint) ZHLN_MESHLET_MAX_VERTICES);
    uint triangleCount  = min(m.triangleCount, (uint) ZHLN_MESHLET_MAX_TRIANGLES);
    SetMeshOutputCounts(vertexCountOut, triangleCount);

    if (gtid < vertexCountOut) {
        outVertices[gtid] = EmitMeshletVertex<ShadowPassOutput, true>(m, inst, instId, gtid, viewId);
    }

    for (uint i = gtid; i < triangleCount; i += 64u) {
        uint3 tri       = inst.fetchMeshletTriangle(m.triangleOffset, i);
        outTriangles[i] = min(tri, uint3(vertexCountOut - 1u));
    }
}

