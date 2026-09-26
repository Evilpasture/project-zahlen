// resources/shaders/basic.slang
import common;
import material_model;
import pbr_helpers;

// The geometry->fragment interfaces live in common.slang: one varying struct
// per pass (ShadowGeometryOutput / GBufferGeometryOutput /
// ForwardGeometryOutput), so the vertex path, the VK_EXT_mesh_shader path
// (basic_mesh.slang) and the fragment shaders below type-check against the
// exact interface each pass rasterises - no preprocessor, no pass defines.
// Each pass compiles its own named entry points.

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
    // Emission is its own attachment. Folding it into SV_Target0 made the
    // deferred lighting pass treat it as reflectance, so an emissive surface
    // went black as soon as nothing was shining on it.
    float4 emissive : SV_Target3;
};

// NOTE: use the Vulkan-native index semantics here. Slang maps SV_InstanceID
// to (InstanceIndex - BaseInstance) -- D3D semantics, i.e. it EXCLUDES
// firstInstance -- whereas the old DXC build received raw InstanceIndex. All
// of this engine's indirect draws encode the instance slot in firstInstance
// (see kGpuCullingSentinel handling below), so the raw Vulkan builtins are
// required. Same argument applies to SV_VertexID/SV_VulkanVertexID.
// Shared scene-vertex payload build: index fetch, skinning, morphing and the
// world transform, identical for every pass. Fills everything EXCEPT the
// clip-space position (each pass selects its own matrix) and, when
// withMotion is true, the motion-vector pair. Instantiations with
// withMotion == false fold the previous-frame skin evaluation away entirely.
SceneGeometryVertex BuildSceneGeometryPayload<bool withMotion>(
    uint vertexId, uint instanceId, out float4 worldPos, out float4 activeVPPos
) {
    SceneGeometryVertex output;

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

    output.tangent.w = localTangent.w;
    output.uv        = localUV;

    output.color           = localColor;
    output.materialIndices = uint4(inst.albedoIndex, inst.normalIndex, inst.pbrIndex, inst.emissiveIndex);
    output.baseColorFactor = inst.baseColorFactor;
    output.emissiveFactor      = inst.emissiveFactor;
    output.pbrFactors          = float3(inst.metallicFactor, inst.roughnessFactor, inst.alphaCutoff);
    output.alphaMode           = inst.alphaMode;
    output.transmissionParams    = inst.transmissionParams();
    output.volumeParams          = inst.volumeParams();
    output.volumeParams.z        = AverageWorldScale(inst.world);
    output.transmissionTextures  = inst.transmissionTextures();

    if (withMotion) {
        float4 prevWorldPos;
        if (inst.isSkinned) {
            prevWorldPos = mul(prevWorldMatrix, SkinPositionPrev(localPos, localJoints, weights, jointOffset));
        } else {
            prevWorldPos = mul(prevWorldMatrix, localPos);
        }
        output.currClip = mul(activeVP, worldPos);
        output.prevClip = mul(scene.frame.prevUnjitteredViewProj, prevWorldPos);
    }

    activeVPPos = mul(activeVP, worldPos);
    return output;
}

[shader("vertex")]
ShadowGeometryOutput VSMainShadow(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID, uint viewId: SV_ViewID) {
    float4 worldPos, activeVPPos;
    SceneGeometryVertex p = BuildSceneGeometryPayload<false>(vertexId, instanceId, worldPos, activeVPPos);
    // Multiview cascades: the shadow render pass fans every draw out to all
    // four cascade layers in one dispatch (viewMask 0x0F), so the light-space
    // matrix -- and the destination layer, implicitly -- comes from ViewIndex.
    p.pos = mul(scene.frame.lightSpaceMatrices[min(viewId, 3u)], worldPos);
    return PackShadowOutput(p);
}

[shader("vertex")]
GBufferGeometryOutput VSMain(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID) {
    float4 worldPos, activeVPPos;
    SceneGeometryVertex p = BuildSceneGeometryPayload<true>(vertexId, instanceId, worldPos, activeVPPos);
    p.pos      = activeVPPos;
    return PackGBufferOutput(p);
}

[shader("vertex")]
ForwardGeometryOutput VSMainForward(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID) {
    float4 worldPos, activeVPPos;
    SceneGeometryVertex p = BuildSceneGeometryPayload<false>(vertexId, instanceId, worldPos, activeVPPos);
    p.pos      = activeVPPos;
    return PackForwardOutput(p);
}

// The three fragment shaders share this module and are type-checked together;
// each consumes exactly the varying struct its pass rasterises (see
// common.slang), so no fragment can accidentally read a dropped varying.
void PSShadow(ShadowGeometryOutput input) {
    uint4 indices          = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float alphaCutoff      = input.pbrFactors.z;
    uint alphaMode         = input.alphaMode;

    float4 albedo = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }
}


PSOutput PSMain(GBufferGeometryOutput input) {
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

    // Allow mirror tier (roughness <0.04) for glossy floors: clamp only to avoid
    // exact zero / division by zero in DistributionGGX, not to 0.045 which forced
    // 0.03 floor into half-res rtr_half path and exposed bilinear bleed white outline.
    float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.001f);
    float metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    if (input.alphaMode == 1) {
        // Alpha-tested: keep at least 0.045 to avoid 0/0 NaN at NdotH=1, but main
        // opaque path can go to 0.001 for mirror tier.
        roughness = max(roughness, 0.045f);
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

    output.color           = float4(albedo.rgb, albedo.a);
    output.emissive        = float4(emissive, 1.0f);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    float currW    = max(input.currClip.w, 0.0001f);
    float prevW    = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}



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

// Khronos rgb_mix: the strongest fresnel channel knocks out the background,
// the colored channels tint the reflection. A plain lerp would leave the
// scene in the weak channels and wash the film toward white.
float3 RgbMix(float3 base, float3 layer, float3 rgbAlpha) {
    float3 w = max(rgbAlpha, 0.0f);
    float wMax = min(max(w.r, max(w.g, w.b)), 1.0f);
    return (1.0f - wMax) * base + w * layer;
}

// The cover mesh has no tangents. A screen-space frame is what the normal
// map was authored against; a constant (1, 0, 0) tangent is not.
float3 PerturbNormal(float3 N, float3 mapNormal, float scale, float3 worldPos, float2 uv) {
    mapNormal = float3(mapNormal.xy * scale, mapNormal.z);
    float3 dp1 = ddx(worldPos);
    float3 dp2 = ddy(worldPos);
    float2 duv1 = ddx(uv);
    float2 duv2 = ddy(uv);
    float3 T = dp1 * duv2.y - dp2 * duv1.y;
    float t2 = dot(T, T);
    if (t2 < 1.0e-8f) {
        return N;
    }
    T = T * rsqrt(t2);
    float3 B = cross(N, T);
    if (dot(cross(T, B), N) < 0.0f) {
        B = -B;
    }
    return SafeNormalize(T * mapNormal.x + B * mapNormal.y + N * mapNormal.z, N);
}

float4 ShadeTransmission(ForwardGeometryOutput input, float3 N, float3 albedo, bool isFrontFace) {
    // glTF doubleSided reverses the back-face normal before lighting. This
    // asset is not double sided: inward shells are back-facing and culled,
    // so the flip does not run on the dish.
    if (!isFrontFace) {
        N = -N;
    }

    float2 screenUV = input.pos.xy / max(scene.frame.screenResolution, float2(1.0f, 1.0f));
    float3 worldPos = ReconstructWorldPos(screenUV, input.pos.z, scene.frame.invViewProj);

    uint4 indices = input.materialIndices;
    if (indices.y != 2u) {
        float3 mapN = scene.globalTextures[indices.y].Sample(scene.defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        N = PerturbNormal(N, mapN, input.volumeParams.w, worldPos, input.uv);
    }

    float3 V = SafeNormalize(scene.frame.camPos.xyz - worldPos, N);
    float NdotV = saturate(dot(N, V));
    float3 R = reflect(-V, N);

    float4 pbr = scene.globalTextures[indices.z].Sample(scene.defaultSampler, input.uv);
    float roughness = max(pbr.g * input.pbrFactors.y, 0.001f);

    float4 params = input.transmissionParams;
    uint4 tex = input.transmissionTextures;
    float iridescence = saturate(params.y);
    if (tex.y != 0xFFFFu) {
        iridescence *= scene.globalTextures[tex.y].Sample(scene.defaultSampler, input.uv).r;
    }
    float thicknessNm = params.z;
    if (tex.x != 0xFFFFu) {
        float g = scene.globalTextures[tex.x].Sample(scene.defaultSampler, input.uv).g;
        thicknessNm = lerp(params.w, params.z, g);
    }

    float3 F0 = float3(0.04f, 0.04f, 0.04f);
    float3 F = FresnelSchlickRoughness(NdotV, F0, roughness);
    if (iridescence > 0.0f && thicknessNm > 0.0f) {
        float3 film = EvalIridescence(1.0f, 1.3f, F0, thicknessNm, NdotV);
        F = lerp(F, film, iridescence);
    }
    F = max(F, 0.0f);

    // Volume thickness is local metres, scaled by the node. Zero keeps the
    // sample on this pixel; a non-zero offset is the rasterizer refraction
    // the thickness texture exists to drive.
    float ior = max(input.volumeParams.y, 1.0001f);
    float thickness = input.volumeParams.x * input.volumeParams.z;
    if (tex.z != 0xFFFFu) {
        thickness *= scene.globalTextures[tex.z].Sample(scene.defaultSampler, input.uv).g;
    }
    float2 refractUV = screenUV;
    if (thickness > 1.0e-5f) {
        float3 bent = refract(-V, N, 1.0f / ior);
        // refract returns 0 on total internal reflection. Keep this pixel then.
        if (dot(bent, bent) > 1.0e-6f) {
            float3 exitPos = worldPos + normalize(bent) * thickness;
            float4 clip = mul(scene.frame.viewProj, float4(exitPos, 1.0f));
            if (clip.w > 0.1f) {
                refractUV = saturate(clip.xy / clip.w * 0.5f + 0.5f);
            }
        }
    }

    float3 transmitted = scene.texTransLighting.SampleLevel(scene.clampSampler, refractUV, 0.0f).rgb * albedo;
    float3 specular = scene.prefilteredMap.SampleLevel(scene.clampSampler, R, roughness * 5.0f).rgb * scene.frame.ambientExposure;
    float transmission = saturate(params.x);
    float3 color = lerp(albedo * scene.frame.ambientExposure * 0.5f, transmitted, transmission);
    color = RgbMix(color, specular, F);
    return float4(min(color, 32768.0f), 1.0f);
}

float4 PSForward(ForwardGeometryOutput input, bool isFrontFace: SV_IsFrontFace) : SV_Target0 {
    uint4 indices          = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float roughnessFactor  = input.pbrFactors.y;

    float4 albedo      = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = scene.globalTextures[indices.w].Sample(scene.defaultSampler, input.uv).rgb;

    float3 emissive = emissiveMap * input.emissiveFactor.rgb * albedo.a;

    if (scene.frame.fullBright != 0) {
        return float4(albedo.rgb + emissive, albedo.a);
    }

    // Read up front so the forward interface keeps the normal and the
    // transmission payload on the ordinary blend path as well.
    float4 params = input.transmissionParams;
    float3 N = SafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (params.x > 0.0f) {
        return ShadeTransmission(input, N, albedo.rgb, isFrontFace);
    }

    // texTransLighting is the opaque-scene copy transmission samples. It is
    // not a reflection, so ordinary blend draws do not add it.
    float3 diffuseIBL = albedo.rgb * scene.frame.ambientExposure * 0.5f;
    float reflFactor  = saturate(1.0f - roughnessFactor);
    float3 finalColor = diffuseIBL + emissive;
    float glassAlpha = albedo.a + (reflFactor * 0.5f);
    return float4(finalColor, saturate(glassAlpha));
}
