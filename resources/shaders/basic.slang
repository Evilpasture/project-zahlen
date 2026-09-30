import common;
import material_model;
import math.transform;
import pbr_helpers;

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
    float4 emissive : SV_Target3;
    float4 clearcoat : SV_Target4;
    float4 anisotropy : SV_Target5;
    float4 sheen : SV_Target6;
};

SceneGeometryVertex BuildSceneGeometryPayload<bool withMotion>(
    uint vertexId, uint instanceId, out float4 worldPos, out float4 activeVPPos
) {
    uint instId       = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
    InstanceData inst = scene.g_instances[instId];

    SceneGeometryVertex output = inst.EvaluateVertex<withMotion>(inst.fetchIndex(vertexId), worldPos);
    output.instanceId = instId;
    float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
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
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, float4(1.0f), input.pbrFactors, input.alphaMode);
    ConfigureSceneMaterial(material, input.uv1, input.instanceId);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }
}

PSOutput PSMain(GBufferGeometryOutput input, bool isFrontFace: SV_IsFrontFace) {
    PSOutput output;
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, input.emissiveFactor, input.pbrFactors, input.alphaMode);
    ConfigureSceneMaterial(material, input.uv1, input.instanceId);
    float4 albedo = SampleMaterialAlbedo<ClearCoatMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }

    if (material.isUnlit) {
        // KHR_materials_unlit uses only baseColorFactor * baseColorTexture *
        // vertex color. Emission, normals, PBR and AO are not shading inputs.
        // Keep the depth/velocity in the opaque G-buffer so glass can sample
        // this surface in the subsequent opaque scene copy.
        float3 safeNormal = SafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
        output.color           = albedo;
        output.emissive        = float4(0.0f);
        output.normalRoughness = float4(PackNormalOctahedron(safeNormal) * 0.5f + 0.5f, 1.0f, 0.0f);
        output.clearcoat       = float4(0.0f);
        output.anisotropy      = float4(0.0f, 0.0f, 1.0f, 1.0f); // Reserved G-buffer unlit marker.
        output.sheen           = float4(0.0f);
    } else {
        // The masked pass used a higher floor than opaque or translucent scene
        // fragments. Keep the floor in the sampling input, not the material type.
        float3 N = normalize(input.normal);
        float3 T = OrthogonalTangent(N, input.tangent.xyz);
        float3 B = cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);
        // glTF double-sided lighting reverses the *entire* shading frame on a
        // backface. Flipping N before computing B would leave the normal-map X
        // axis unflipped; flip all three axes after constructing the front frame.
        float facing = isFrontFace ? 1.0f : -1.0f;
        material.roughnessFloor     = input.alphaMode == 1 ? 0.045f : 0.001f;
        material.hasTangent         = any(input.tangent.xyz);
        material.geometricNormal    = N * facing;
        material.tangentDirection   = T * facing;
        material.bitangentDirection = B * facing;
        material.coatParams         = input.coatParams;
        material.coatTextures       = input.coatTextures;
        material.anisotropyParams   = input.anisotropyParams;
        material.anisotropyTexIndex = input.anisotropyTexIndex;

        float3 emissive = SampleMaterialEmissive<ClearCoatMaterial>(material);
        SurfaceInteraction surface = EvaluateMaterial<ClearCoatMaterial>(material, albedo, emissive);
        output.color           = float4(surface.albedo, surface.alpha);
        output.emissive        = float4(surface.emissive, 1.0f);
        float2 packedNormal    = PackNormalOctahedron(surface.normal) * 0.5f + 0.5f;
        output.normalRoughness = float4(packedNormal, surface.roughness, surface.metallic);
        output.clearcoat       = surface.clearcoat;
        output.anisotropy      = float4(surface.anisotropy.xy, EncodeGBufferAnisotropy(surface.anisotropy.z), surface.anisotropy.w);
        output.sheen           = surface.sheen;
    }

    float currW    = max(input.currClip.w, 0.0001f);
    float prevW    = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}

float3 RgbMix(float3 base, float3 layer, float3 rgbAlpha) {
    float3 w = max(rgbAlpha, 0.0f);
    float wMax = min(max(w.r, max(w.g, w.b)), 1.0f);
    return (1.0f - wMax) * base + w * layer;
}

float4 ShadeTransmission(ForwardGeometryOutput input, float3 N, float3 albedo, bool isFrontFace) {
    if (!isFrontFace) {
        N = -N;
    }

    float2 screenUV = input.pos.xy / max(scene.frame.screenResolution, float2(1.0f, 1.0f));
    float3 worldPos = ReconstructWorldPos(screenUV, input.pos.z, scene.frame.invViewProj);

    uint4 indices = input.materialIndices;
    if (indices.y != 2u) {
        float3 mapN = SampleMaterialTexture(indices.y, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerNormal), input.samplerCodes, kSamplerNormal).rgb * 2.0f - 1.0f;
        // Account for rotated/mirrored UVs before reconstructing the tangent frame.
        MaterialSampleInputs normalInput = MakeMaterialInputs(input.uv, input.color, indices, input.samplerCodes,
                                                               input.baseColorFactor, input.emissiveFactor, input.pbrFactors, 0u);
        ConfigureSceneMaterial(normalInput, input.uv1, input.instanceId);
        mapN = TransformNormalSample(mapN, normalInput, kSamplerNormal);
        N = PerturbNormal(N, mapN, input.volumeParams.w, worldPos,
                          SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerNormal));
    }

    float3 V = SafeNormalize(scene.frame.camPos.xyz - worldPos, N);
    float NdotV = saturate(dot(N, V));

    float4 pbr = SampleMaterialTexture(indices.z, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerPbr), input.samplerCodes, kSamplerPbr);
    float roughness = max(pbr.g * input.pbrFactors.y, 0.001f);
    float metallic  = saturate(pbr.b * input.pbrFactors.x);

    float4 params = input.transmissionParams;
    uint4 tex = input.transmissionTextures;
    float iridescence = saturate(params.y);
    if (tex.y != 0xFFFFu) {
        iridescence *= SampleMaterialTexture(tex.y, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerIridescence), input.samplerCodes, kSamplerIridescence).r;
    }
    float thicknessNm = params.z;
    if (tex.x != 0xFFFFu) {
        float g = SampleMaterialTexture(tex.x, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerFilmThickness), input.samplerCodes, kSamplerFilmThickness).g;
        thicknessNm = lerp(params.w, params.z, g);
    }

    float ior = max(input.volumeParams.y, 1.0f);
    float eta = (ior - 1.0f) / (ior + 1.0f);
    float3 F0 = lerp(float3(eta * eta), albedo, metallic);
    // A metal reflects rather than transmitting; at IOR 1 the dielectric
    // component alone has no index boundary and must not reflect air.
    float3 F = (ior > 1.0f || metallic > 0.0f) ? FresnelSchlickRoughness(NdotV, F0, roughness) : float3(0.0f);
    if (iridescence > 0.0f && thicknessNm > 0.0f) {
        float3 film = EvalIridescence(1.0f, 1.3f, F0, thicknessNm, NdotV);
        F = lerp(F, film, iridescence);
    }
    F = max(F, 0.0f);

    float thickness = input.volumeParams.x * input.volumeParams.z;
    if (tex.z != 0xFFFFu) {
        thickness *= SampleMaterialTexture(tex.z, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerVolumeThickness), input.samplerCodes, kSamplerVolumeThickness).g;
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

    // A zero-thickness (thin-walled) surface still scatters transmitted light.
    // No index boundary at IOR 1 means no microfacet refraction, at any roughness.
    float transRoughness = roughness * saturate(2.0f * (ior - 1.0f));
    // The frame and transmission target share an extent. The sampler clamps
    // the fractional max LOD to the last mip for non-power-of-two targets.
    float maxLod = log2(max(scene.frame.screenResolution.x, scene.frame.screenResolution.y));
    float lod = transRoughness * maxLod;
    // A metallic surface absorbs refracted light. The extension's linear R
    // channel independently modulates optical transmission, not alpha coverage.
    float3 transmitted = scene.texTransLighting.SampleLevel(scene.clampSampler, refractUV, lod).rgb * albedo * (1.0f - metallic);
    float3 T = OrthogonalTangent(N, input.tangent.xyz);
    float3 B = cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);
    float4 aniso = SampleMaterialAnisotropy(N, T, B, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerAnisotropy), input.anisotropyParams, input.anisotropyTexIndex, input.samplerCodes);
    float3 anisotropicT = aniso.z > 0.0f ? UnpackAnisotropy(aniso.xy, N) : T;
    float3 R = AnisotropicReflection(N, V, cross(N, anisotropicT), roughness, aniso.z);
    float3 specular = SampleAnisotropicEnvironment(scene.prefilteredMap, scene.clampSampler, R, roughness, aniso.z) *
                      scene.frame.ambientExposure;
    float transmission = saturate(params.x);
    if (tex.w != 0xFFFFu) {
        transmission *= SampleMaterialTexture(tex.w, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerTransmission),
                                              input.samplerCodes, kSamplerTransmission).r;
    }
    float3 diffuse = albedo * (1.0f - metallic) * scene.frame.ambientExposure * 0.5f;
    float3 color = lerp(diffuse, transmitted, transmission);
    color = RgbMix(color, specular, F);
    return float4(min(color, 32768.0f), 1.0f);
}

float4 PSForward(ForwardGeometryOutput input, bool isFrontFace: SV_IsFrontFace) : SV_Target0 {
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, input.emissiveFactor, input.pbrFactors, input.alphaMode);
    ConfigureSceneMaterial(material, input.uv1, input.instanceId);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }
    // BLEND still composites by coverage; OPAQUE and MASK write full alpha.
    // None of the lit paths (including emissive or fullBright) apply to unlit.
    if (material.isUnlit) {
        return float4(albedo.rgb, material.alphaMode == 2u ? albedo.a : 1.0f);
    }
    float3 sampledEmissive = SampleMaterialEmissive<StandardPBRMaterial>(material);
    float3 emissive = sampledEmissive * albedo.a;

    if (scene.frame.fullBright != 0) {
        return float4(albedo.rgb + emissive, input.alphaMode == 2u ? albedo.a : 1.0f);
    }

    float4 params = input.transmissionParams;
    float3 N = SafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f));
    if (params.x > 0.0f) {
        float4 color = ShadeTransmission(input, N, albedo.rgb, isFrontFace);
        color.a = input.alphaMode == 2u ? albedo.a : 1.0f;
        return color;
    }

    // Transmission orients its normal inside ShadeTransmission. Non-transmitting
    // two-sided surfaces need the same backface rule for both Fresnel and the
    // anisotropic tangent frame (as in the deferred pass).
    float3 T = OrthogonalTangent(N, input.tangent.xyz);
    float3 B = cross(N, T) * (input.tangent.w < 0.0f ? -1.0f : 1.0f);
    float facing = isFrontFace ? 1.0f : -1.0f;
    N *= facing;
    T *= facing;
    B *= facing;

    float2 screenUV = input.pos.xy / max(scene.frame.screenResolution, float2(1.0f, 1.0f));
    float3 worldPos = ReconstructWorldPos(screenUV, input.pos.z, scene.frame.invViewProj);
    float3 V        = SafeNormalize(scene.frame.camPos.xyz - worldPos, N);
    float NdotV     = saturate(dot(N, V));
    // Forward shading uses the interpolated normal; its transmission branch
    // applies a separate derivative-based normal map before reaching here.
    material.geometricNormal = N;
    SurfaceInteraction surface = EvaluateMaterial<StandardPBRMaterial>(material, albedo, sampledEmissive);
    float roughness = surface.roughness;
    float metallic  = surface.metallic;
    float4 aniso = SampleMaterialAnisotropy(N, T, B, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerAnisotropy), input.anisotropyParams, input.anisotropyTexIndex, input.samplerCodes);
    float3 anisotropicT = aniso.z > 0.0f ? UnpackAnisotropy(aniso.xy, N) : T;
    float3 anisotropicB = cross(N, anisotropicT);
    float3 R = AnisotropicReflection(N, V, anisotropicB, roughness, aniso.z);
    float3 F0       = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallic);
    float3 F_rough  = FresnelSchlickRoughness(NdotV, F0, roughness);
    float2 envBRDF  = scene.brdfLUT.SampleLevel(scene.clampSampler, float2(NdotV, roughness), 0.0f).rg;
    // The split-sum LUT already integrates Schlick's angle-dependent Fresnel.
    // Its A coefficient multiplies F0, not a second grazing-angle evaluation.
    float3 FssEss   = F0 * envBRDF.x + float3(envBRDF.y, envBRDF.y, envBRDF.y);
    float3 prefiltered = SampleAnisotropicEnvironment(scene.prefilteredMap, scene.clampSampler, R, roughness, aniso.z);
    float3 specularIBL = prefiltered * scene.frame.ambientExposure * FssEss;
    float3 diffuseIBL  = albedo.rgb * (1.0f - F_rough) * (1.0f - metallic) * scene.frame.ambientExposure * 0.5f;
    float3 litBase = (diffuseIBL + specularIBL) * surface.occlusion;
    float3 sheenColor = surface.sheen.rgb;
    if (any(sheenColor > 0.0f)) {
        float attenuation = SheenAlbedoScaling(sheenColor, surface.sheen.a, NdotV, NdotV);
        float3 sheenEnv = scene.prefilteredMap.SampleLevel(scene.clampSampler, reflect(-V, N), max(surface.sheen.a, 0.2f) * 5.0f).rgb;
        litBase = litBase * attenuation + sheenEnv * scene.frame.ambientExposure *
                  SheenEnvironmentWeight(sheenColor, NdotV) * surface.occlusion;
    }
    return float4(litBase + emissive, albedo.a);
}
