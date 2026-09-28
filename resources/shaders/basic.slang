import common;
import material_model;
import pbr_helpers;


struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
    float4 emissive : SV_Target3;
    float4 clearcoat : SV_Target4;
    float4 anisotropy : SV_Target5;
};

SceneGeometryVertex BuildSceneGeometryPayload<bool withMotion>(
    uint vertexId, uint instanceId, out float4 worldPos, out float4 activeVPPos
) {
    SceneGeometryVertex output;

    uint instId       = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
    InstanceData inst = scene.g_instances[instId];

    uint actualVertexId = inst.fetchIndex(vertexId);
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
    output.coatParams            = float4(inst.clearcoatFactor(), inst.clearcoatRoughnessFactor(), inst.clearcoatNormalScale(), 0.0f);
    output.coatTextures          = uint4(inst.clearcoatTexIndex(), inst.clearcoatRoughnessTexIndex(), inst.clearcoatNormalTexIndex(), 0u);
    output.anisotropyParams      = float2(inst.anisotropyStrength, inst.anisotropyRotation);
    output.anisotropyTexIndex    = inst.anisotropyTexIndex;

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

// glTF's optional RG channels are a direction in tangent space and B masks
// strength. Rotation is in radians; the no-texture direction is (1, 0).
float4 SampleMaterialAnisotropy(float3 N, float3 T, float3 B, float2 uv, float2 params, uint texIndex) {
    float strength = saturate(params.x);
    if (strength <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float2 direction = float2(1.0f, 0.0f);
    if (texIndex != 0xFFFFu) {
        float3 sample = scene.globalTextures[texIndex].Sample(scene.defaultSampler, uv).rgb;
        direction = sample.rg * 2.0f - 1.0f;
        float lenSq = dot(direction, direction);
        direction = lenSq > 1e-6f ? direction * rsqrt(lenSq) : float2(1.0f, 0.0f);
        strength *= sample.b;
    }
    float s = sin(params.y);
    float c = cos(params.y);
    float2 rotated = float2(c * direction.x - s * direction.y, s * direction.x + c * direction.y);
    return PackAnisotropy(N, rotated.x * T + rotated.y * B, strength);
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

    float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.001f);
    float metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    if (input.alphaMode == 1) {
        roughness = max(roughness, 0.045f);
    }

    float3 geometric = normalize(input.normal);
    float3 worldNormal = geometric;
    float3 coatNormal  = geometric;
    bool hasTangent = any(input.tangent.xyz);
    float3 T = OrthogonalTangent(geometric, input.tangent.xyz);
    float3 B = cross(geometric, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);

    if (indices.y != 2 && hasTangent) {
        float3 normalMap = scene.globalTextures[indices.y].Sample(scene.defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        worldNormal      = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * geometric);
    }

    float coatFactor = input.coatParams.x;
    float coatRough  = input.coatParams.y;
    if (coatFactor > 0.0f && input.coatTextures.x != 0xFFFFu) {
        coatFactor *= scene.globalTextures[input.coatTextures.x].Sample(scene.defaultSampler, input.uv).r;
    }
    if (coatFactor > 0.0f && input.coatTextures.y != 0xFFFFu) {
        coatRough *= scene.globalTextures[input.coatTextures.y].Sample(scene.defaultSampler, input.uv).g;
    }
    coatFactor = saturate(coatFactor);
    coatRough  = saturate(coatRough);
    if (coatFactor > 1.0f / 255.0f && input.coatTextures.z != 0xFFFFu && hasTangent) {
        float3 mapN = scene.globalTextures[input.coatTextures.z].Sample(scene.defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        mapN.xy *= input.coatParams.z;
        coatNormal = normalize(mapN.x * T + mapN.y * B + mapN.z * geometric);
    }
    float2 packedCoat = PackNormalOctahedron(coatNormal) * 0.5f + 0.5f;
    output.clearcoat  = (coatFactor > 1.0f / 255.0f) ? float4(packedCoat, coatRough, coatFactor) : float4(0.0f, 0.0f, 0.0f, 0.0f);

    output.color           = float4(albedo.rgb, albedo.a);
    output.emissive        = float4(emissive, 1.0f);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);
    output.anisotropy = SampleMaterialAnisotropy(worldNormal, T, B, input.uv, input.anisotropyParams, input.anisotropyTexIndex);

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

float3 RgbMix(float3 base, float3 layer, float3 rgbAlpha) {
    float3 w = max(rgbAlpha, 0.0f);
    float wMax = min(max(w.r, max(w.g, w.b)), 1.0f);
    return (1.0f - wMax) * base + w * layer;
}

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

    float ior = max(input.volumeParams.y, 1.0001f);
    float thickness = input.volumeParams.x * input.volumeParams.z;
    if (tex.z != 0xFFFFu) {
        thickness *= scene.globalTextures[tex.z].Sample(scene.defaultSampler, input.uv).g;
    }
    float2 refractUV = screenUV;
    if (thickness > 1.0e-5f) {
        float3 bent = refract(-V, N, 1.0f / ior);
        if (dot(bent, bent) > 1.0e-6f) {
            float3 exitPos = worldPos + normalize(bent) * thickness;
            float4 clip = mul(scene.frame.viewProj, float4(exitPos, 1.0f));
            if (clip.w > 0.1f) {
                refractUV = saturate(clip.xy / clip.w * 0.5f + 0.5f);
            }
        }
    }

    float3 transmitted = scene.texTransLighting.SampleLevel(scene.clampSampler, refractUV, 0.0f).rgb * albedo;
    float3 T = OrthogonalTangent(N, input.tangent.xyz);
    float3 B = cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);
    float4 aniso = SampleMaterialAnisotropy(N, T, B, input.uv, input.anisotropyParams, input.anisotropyTexIndex);
    float3 anisotropicT = aniso.z > 0.0f ? UnpackAnisotropy(aniso.xy, N) : T;
    float3 R = AnisotropicReflection(N, V, cross(N, anisotropicT), roughness, aniso.z);
    float3 specular = SampleAnisotropicEnvironment(scene.prefilteredMap, scene.clampSampler, R, roughness, aniso.z) *
                      scene.frame.ambientExposure;
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

    float4 params = input.transmissionParams;
    float3 N = SafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (params.x > 0.0f) {
        return ShadeTransmission(input, N, albedo.rgb, isFrontFace);
    }

    float2 screenUV = input.pos.xy / max(scene.frame.screenResolution, float2(1.0f, 1.0f));
    float3 worldPos = ReconstructWorldPos(screenUV, input.pos.z, scene.frame.invViewProj);
    float3 V        = SafeNormalize(scene.frame.camPos.xyz - worldPos, N);
    float NdotV     = saturate(dot(N, V));
    float4 pbr      = scene.globalTextures[indices.z].Sample(scene.defaultSampler, input.uv);
    float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.001f);
    float metallic  = (indices.z == 0 ? 1.0f : pbr.b) * input.pbrFactors.x;
    float3 T = OrthogonalTangent(N, input.tangent.xyz);
    float3 B = cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);
    float4 aniso = SampleMaterialAnisotropy(N, T, B, input.uv, input.anisotropyParams, input.anisotropyTexIndex);
    float3 anisotropicT = aniso.z > 0.0f ? UnpackAnisotropy(aniso.xy, N) : T;
    float3 anisotropicB = cross(N, anisotropicT);
    float3 R = AnisotropicReflection(N, V, anisotropicB, roughness, aniso.z);
    float3 F0       = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallic);
    float3 F_rough  = FresnelSchlickRoughness(NdotV, F0, roughness);
    float2 envBRDF  = scene.brdfLUT.SampleLevel(scene.clampSampler, float2(NdotV, roughness), 0.0f).rg;
    float3 FssEss   = F_rough * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);
    float3 prefiltered = SampleAnisotropicEnvironment(scene.prefilteredMap, scene.clampSampler, R, roughness, aniso.z);
    float3 specularIBL = prefiltered * scene.frame.ambientExposure * FssEss;
    float3 diffuseIBL  = albedo.rgb * (1.0f - F_rough) * (1.0f - metallic) * scene.frame.ambientExposure * 0.5f;
    return float4(diffuseIBL + specularIBL + emissive, albedo.a);
}
