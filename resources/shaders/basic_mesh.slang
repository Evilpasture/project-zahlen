// resources/shaders/basic_mesh.slang
//
// VK_EXT_mesh_shader mesh stage. One workgroup emits exactly one meshlet:
//   * 64 threads, one output vertex each (kMeshletMaxVertices == 64)
//   * up to 124 primitives, 2 per thread (kMeshletMaxTriangles == 124)
//
// The vertex work is a verbatim port of basic.slang:VSMain -- same skinning,
// morphing, viewmodel/shadow matrix selection and material unpacking -- so the
// fragment shaders (PSMain / PSShadow / PSForward) are shared unchanged between
// the vertex and the mesh path, and the two paths must produce identical
// pixels. VSOutput itself is declared once in common.slang.

import common;

#ifndef ZHLN_MESHLET_MAX_VERTICES
#define ZHLN_MESHLET_MAX_VERTICES 64
#endif

#ifndef ZHLN_MESHLET_MAX_TRIANGLES
#define ZHLN_MESHLET_MAX_TRIANGLES 124
#endif

[shader("mesh")]
[outputtopology("triangle")]
[numthreads(64, 1, 1)]
void MeshMain(
    uint gtid: SV_GroupIndex,
    uint3 gid: SV_GroupID,
    in payload MeshletTaskPayload payload,
    out vertices SceneGeometryOutput outVertices[ZHLN_MESHLET_MAX_VERTICES],
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

    // ------------------------------------------------------------------
    // 1. Cooperative vertex fetch + transform (one vertex per thread)
    // ------------------------------------------------------------------
    if (gtid < vertexCountOut) {
        uint actualVertexId = inst.fetchMeshletVertex(m.vertexOffset, gtid);
        actualVertexId      = min(actualVertexId, max(inst.vertexCount, 1u) - 1u);

        SceneGeometryVertex output;

        float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;

        float3 position     = inst.fetchPosition(actualVertexId);
        float2 localUV      = float2(0, 0);
        float4 localColor   = float4(1, 1, 1, 1);
        float3 localNormal  = float3(0, 1, 0);
        float4 localTangent = float4(1, 0, 0, 1);

        if (inst.attrAddress != 0) {
            VertexAttributes attributes = inst.fetchAttributes(actualVertexId);
            localNormal                 = attributes.unpackedNormal.xyz;
            localTangent                = attributes.unpackedTangent;
            localUV                     = attributes.unpackedUV;
            localColor                  = attributes.unpackedColor;
        }

        float4 localPos = float4(position, 1.0f);

        uint4 localJoints = uint4(0, 0, 0, 0);
        float4 weights    = float4(0.0f, 0.0f, 0.0f, 0.0f);

        if (inst.isSkinned && inst.skinAddress != 0) {
            SkinVertex skin = inst.fetchSkin(actualVertexId);
            localJoints     = skin.unpackedJoints;
            weights         = skin.unpackedWeights;
        }

        if (inst.activeMorphCount > 0) {
            localPos.xyz += GetMorphDisplacement(actualVertexId, inst.vertexCount, inst.morphOffset, inst.activeMorphCount, inst.morphWeights);
        }

        float4 worldPos;
        float3x3 world3x3        = (float3x3) inst.world;
        float4x4 worldMatrix     = inst.world;
        float4x4 prevWorldMatrix = inst.prevWorld;
        uint jointOffset         = inst.jointOffset;

        if (inst.isSkinned) {
            worldPos           = mul(worldMatrix, SkinPosition(localPos, localJoints, weights, jointOffset));
            output.normal      = normalize(mul(world3x3, SkinDirection(localNormal, localJoints, weights, jointOffset)));
            output.tangent.xyz = normalize(mul(world3x3, SkinDirection(localTangent.xyz, localJoints, weights, jointOffset)));
        } else {
            worldPos           = mul(worldMatrix, localPos);
            output.normal      = normalize(mul(world3x3, localNormal));
            output.tangent.xyz = normalize(mul(world3x3, localTangent.xyz));
        }

        float4 baseColorFactor = inst.baseColorFactor;
        float4 emissiveFactor  = inst.emissiveFactor;
        float metallicFactor   = inst.metallicFactor;
        float roughnessFactor  = inst.roughnessFactor;
        float alphaCutoff      = inst.alphaCutoff;

        if (obj.isShadowPass != 0) {
            uint cascadeIndex = obj.isShadowPass - 1;

            output.pos             = mul(scene.frame.lightSpaceMatrices[cascadeIndex], worldPos);
            output.uv              = localUV;
            output.baseColorFactor = baseColorFactor;
            output.emissiveFactor  = emissiveFactor;
            output.pbrFactors      = float3(metallicFactor, roughnessFactor, alphaCutoff);
            output.alphaMode       = inst.alphaMode;
            output.materialIndices = uint4(inst.albedoIndex, 0, 0, 0);

            output.currClip = 0;
            output.prevClip = 0;
            output.normal   = 0;
            output.tangent  = 0;
            output.color    = localColor;
        } else {
            output.pos      = mul(activeVP, worldPos);
            output.currClip = mul(activeVP, worldPos);

            float4 prevWorldPos;
            if (inst.isSkinned) {
                prevWorldPos = mul(prevWorldMatrix, SkinPositionPrev(localPos, localJoints, weights, jointOffset));
            } else {
                prevWorldPos = mul(prevWorldMatrix, localPos);
            }
            output.prevClip = mul(scene.frame.prevUnjitteredViewProj, prevWorldPos);

            output.tangent.w = localTangent.w;
            output.uv        = localUV;

            output.color           = localColor;
            output.materialIndices = uint4(inst.albedoIndex, inst.normalIndex, inst.pbrIndex, inst.emissiveIndex);
            output.baseColorFactor = baseColorFactor;
            output.emissiveFactor  = emissiveFactor;
            output.pbrFactors      = float3(metallicFactor, roughnessFactor, alphaCutoff);
            output.alphaMode       = inst.alphaMode;
        }

        outVertices[gtid] = PackGeometryOutput(output);
    }

    // ------------------------------------------------------------------
    // 2. Cooperative micro-index assembly (124 prims / 64 threads => 2 each)
    // ------------------------------------------------------------------
    for (uint i = gtid; i < triangleCount; i += 64u) {
        uint3 tri      = inst.fetchMeshletTriangle(m.triangleOffset, i);
        outTriangles[i] = min(tri, uint3(vertexCountOut - 1u));
    }
}
