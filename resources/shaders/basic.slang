// resources/shaders/basic.slang
import common;
import pbr_helpers;

// The geometry->fragment interface now lives in common.slang so the vertex path
// (VSMain below) and the VK_EXT_mesh_shader path (basic_mesh.slang: MeshMain)
// feed the fragment shaders in this file through one single, shared
// declaration. The local alias keeps this file (and its diffs) unchanged.
typealias VSOutput = SceneGeometryOutput;

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
};

// NOTE: use the Vulkan-native index semantics here. Slang maps SV_InstanceID
// to (InstanceIndex - BaseInstance) -- D3D semantics, i.e. it EXCLUDES
// firstInstance -- whereas the old DXC build received raw InstanceIndex. All
// of this engine's indirect draws encode the instance slot in firstInstance
// (see kGpuCullingSentinel handling below), so the raw Vulkan builtins are
// required. Same argument applies to SV_VertexID/SV_VulkanVertexID.
VSOutput VSMain(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID, uint viewId: SV_ViewID) {
    VSOutput output;

    uint instId       = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
    InstanceData inst = scene.g_instances[instId];

    // Typed BDA indexing keeps the byte-stride contract in one place.
    uint actualVertexId = inst.fetchIndex(vertexId);
    // Preserve the legacy defensive clamp before any BDA vertex fetch.
    actualVertexId = min(actualVertexId, max(inst.vertexCount, 1u) - 1u);

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

    uint vertexCount      = inst.vertexCount;
    uint morphOffset      = inst.morphOffset;
    uint activeMorphCount = inst.activeMorphCount;
    float4 morphWeights   = inst.morphWeights;

    if (activeMorphCount > 0) {
        localPos.xyz += GetMorphDisplacement(actualVertexId, vertexCount, morphOffset, activeMorphCount, morphWeights);
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
        // Extract the cascade index mapped from the CPU
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
        return output;
    }

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
    return output;
}

void PSShadow(VSOutput input) {
    uint4 indices          = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float alphaCutoff      = input.pbrFactors.z;
    uint alphaMode         = input.alphaMode;

    float4 albedo = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }
}

PSOutput PSMain(VSOutput input) {
    PSOutput output;

    uint4 indices          = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float metallicFactor   = input.pbrFactors.x;
    float roughnessFactor  = input.pbrFactors.y;
    float alphaCutoff      = input.pbrFactors.z;
    uint alphaMode         = input.alphaMode;

    float4 albedo      = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = scene.globalTextures[indices.w].Sample(scene.defaultSampler, input.uv).rgb;
    float3 emissive    = emissiveMap * input.emissiveFactor.rgb;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }

    float4 pbr = scene.globalTextures[indices.z].Sample(scene.defaultSampler, input.uv);

    float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
    float metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    if (input.alphaMode == 1) {
        roughness = 0.0f;
    }

    float3 N           = normalize(input.normal);
    float3 worldNormal = N;

    if (indices.y != 2 && any(input.tangent.xyz)) {
        float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
        if (dot(T_unnorm, T_unnorm) < 0.0001f) {
            T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
        }
        float3 T          = normalize(T_unnorm);
        float tangentSign = input.tangent.w;
        float3 B          = normalize(cross(N, T) * tangentSign);

        float3 normalMap = scene.globalTextures[indices.y].Sample(scene.defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        worldNormal      = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
    }

    output.color           = float4(albedo.rgb + emissive, albedo.a);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    float currW    = max(input.currClip.w, 0.0001f);
    float prevW    = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}

#ifdef FORWARD_PASS

float3 RotateVector(float3 V, float3 lightDir) {
    float lenB = length(lightDir);
    if (lenB < 0.001f)
        return V;
    float3 A     = normalize(float3(0.5f, 1.0f, 0.2f));
    float3 B     = lightDir / lenB;
    float3 q_xyz = cross(B, A);
    float q_w    = 1.0f + dot(A, B);
    float q_len  = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
    if (q_len > 0.0001f) {
        q_xyz /= q_len;
        q_w /= q_len;
        return V + 2.0f * cross(q_xyz, cross(q_xyz, V) + q_w * V);
    }
    return V;
}

float4 PSForward(VSOutput input) : SV_Target0 {
    uint4 indices          = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float roughnessFactor  = input.pbrFactors.y;

    float4 albedo      = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = scene.globalTextures[indices.w].Sample(scene.defaultSampler, input.uv).rgb;

    float3 emissive = emissiveMap * input.emissiveFactor.rgb * albedo.a;

    if (scene.frame.fullBright != 0) {
        return float4(albedo.rgb + emissive, albedo.a);
    }

    float2 screenUV = input.pos.xy / scene.frame.screenResolution;

    float4 transLighting = scene.texTransLighting.SampleLevel(scene.defaultSampler, screenUV, 0);

    float3 diffuseIBL = albedo.rgb * scene.frame.ambientExposure * 0.5f;

    float reflFactor  = saturate(1.0f - roughnessFactor);
    float3 finalColor = diffuseIBL + (transLighting.rgb * reflFactor) + emissive;

    float glassAlpha = albedo.a + (reflFactor * 0.5f);

    return float4(finalColor, saturate(glassAlpha));
}
#endif
