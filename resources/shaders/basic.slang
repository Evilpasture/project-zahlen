import common;
import material_model;
import math.transform;
import pbr_helpers;

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
};

SceneGeometryVertex BuildSceneGeometryPayload<bool withMotion>(
    uint vertexId, uint instanceId, out float4 worldPos, out float4 activeVPPos
) {
    uint instId       = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
    InstanceData inst = scene.g_instances[instId];

    SceneGeometryVertex output = inst.EvaluateVertex<withMotion>(inst.fetchRasterIndex(vertexId), worldPos);
    output.instanceId = instId;
    float4x4 activeVP = inst.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
    activeVPPos = inst.BandedClipDepth(mul(activeVP, worldPos));
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

[shader("vertex")]
EmissiveGeometryOutput VSMainEmissive(uint vertexId: SV_VulkanVertexID, uint instanceId: SV_VulkanInstanceID) {
    float4 worldPos, activeVPPos;
    SceneGeometryVertex p = BuildSceneGeometryPayload<false>(vertexId, instanceId, worldPos, activeVPPos);
    p.pos      = activeVPPos;
    return PackEmissiveOutput(p);
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

// KHR_materials_specular: the fragment's dielectric response, resolved from
// the instance's authored factors plus the one texture slot the material
// layout has left for the extension. That slot holds specularTexture.a and/or
// specularColorTexture.rgb depending on the mode the importer picked; the
// color half is sRGB-encoded and already decoded by the sampler.
struct DielectricResponse {
    float3 f0;  // dielectric reflectance, already clamped and scaled
    float  f90; // grazing reflectance (the extension's specularFactor)
};

DielectricResponse ResolveDielectricResponse(InstanceData inst, float2 uv, float2 uv1, uint instanceId) {
    float3 color  = inst.specularColorFactor;
    float  factor = inst.specularFactor;
    if (inst.specularTexIndex != 0xFFFFu) {
        uint2  codes = uint2(inst.samplerCodes0, inst.samplerCodes1);
        float4 texel = SampleMaterialTexture(
            inst.specularTexIndex, SceneTextureUV(uv, uv1, instanceId, kSamplerSpecular), codes, kSamplerSpecular
        );
        // Mode 1 binds specularTexture.a alone, mode 2 specularColorTexture.rgb
        // alone; anything unmapped keeps its authored factor. The unused half
        // is never read as data, as the extension's texture definition asks.
        if (inst.specularTextureMode != 1u) {
            color *= texel.rgb;
        }
        if (inst.specularTextureMode != 2u) {
            factor *= texel.a;
        }
    }

    // transmissionIor is the authored KHR_materials_ior for every material,
    // transmitting or not: the importer writes it unconditionally and defaults
    // it to 1.5, so it is the right source for eta here. volumeParams() is not
    // -- it substitutes 1.5 for any opaque material, which is exactly the case
    // this extension changes.
    float ior = max(inst.transmissionIor, 1.0f);
    float eta = (ior - 1.0f) / (ior + 1.0f);

    DielectricResponse response;
    // The extension clamps the IOR reflectance to 1 *before* applying the
    // factor: the factor scales the response, it is not a reflectance itself.
    response.f0  = min(float3(eta * eta) * color, float3(1.0f)) * factor;
    response.f90 = factor;
    return response;
}

PSOutput PSMain(GBufferGeometryOutput input, bool isFrontFace: SV_IsFrontFace) {
    PSOutput output;
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, float4(0.0f), input.pbrFactors, input.alphaMode);
    ConfigureSceneMaterial(material, input.uv1, input.instanceId);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
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
        output.normalRoughness = float4(PackNormalOctahedron(safeNormal) * 0.5f + 0.5f, 1.0f, 0.0f);
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
        SurfaceInteraction surface = EvaluateMaterial<StandardPBRMaterial>(material, albedo, float3(0.0f));
        // Source VRAD is composited in the dedicated additive baked-lightmap
        // pass below. Suppress only this material's dynamic diffuse input here
        // so the clustered path cannot multiply the authored bake by ambient
        // again; normals/roughness/depth remain available to the rest of the
        // deferred renderer until the stencil compute-skip stage lands.
        if (scene.g_instances[input.instanceId].hasBakedIrradiance) {
            surface.albedo = float3(0.0f);
        }
        output.color           = float4(surface.albedo, surface.alpha);
        float2 packedNormal    = PackNormalOctahedron(surface.normal) * 0.5f + 0.5f;
        output.normalRoughness = float4(packedNormal, surface.roughness, surface.metallic);
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
    // KHR_materials_specular replaces the dielectric reflectance the IOR
    // implies and carries the grazing value the plain Schlick form cannot:
    // f90 is the material's reflectance at 90 degrees, not 1.
    float3       dielectricF0 = float3(eta * eta);
    float        f90          = 1.0f;
    InstanceData inst         = scene.g_instances[input.instanceId];
    if (inst.hasSpecularResponse) {
        DielectricResponse response = ResolveDielectricResponse(inst, input.uv, input.uv1, input.instanceId);
        dielectricF0                = response.f0;
        f90                         = response.f90;
    }
    float3 F0 = lerp(dielectricF0, albedo, metallic);
    // A metal reflects rather than transmitting; at IOR 1 the dielectric
    // component alone has no index boundary and must not reflect air.
    float3 F = (ior > 1.0f || metallic > 0.0f) ? SpecularFresnelRoughness(NdotV, F0, EffectiveF90(f90, metallic), roughness) : float3(0.0f);
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
    float3 diffuse = albedo * (1.0f - metallic) * EvaluateSH(N, scene.frame.sh) * scene.frame.ambientExposure;
    float3 color = lerp(diffuse, transmitted, transmission);
    color = RgbMix(color, specular, F);
    return float4(min(color, 32768.0f), 1.0f);
}

// Lit emitters still own a compact G-buffer record. This second raster pass
// adds just their HDR radiance to LightingGI after deferred lighting, before
// screen-space reflections consume that lighting. It deliberately shares the
// alpha-mask test with PSMain/PSForward so discarded texels never contribute.
float4 PSEmissive(EmissiveGeometryOutput input) : SV_Target0 {
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, input.emissiveFactor, input.pbrFactors, input.alphaMode);
    ConfigureSceneMaterial(material, input.uv1, input.instanceId);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }
    const float3 materialEmission = SampleMaterialEmissive<StandardPBRMaterial>(material);
    // Baked irradiance is a diffuse-light transport result, not material
    // emission. This additive overlay is merely the compact-G-buffer transport
    // mechanism: it restores albedo × VRAD after deferred lighting has kept
    // the lightmapped material's diffuse input at zero.
    const float3 bakedDiffuse = albedo.rgb * SampleBakedIrradiance(input.uv1, SafeNormalize(input.normal, float3(0.0f, 1.0f, 0.0f)), input.instanceId);
    return float4((materialEmission + bakedDiffuse) * albedo.a, 0.0f);
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
    // Unlit materials are forward-only. Their base color and emission are both
    // self-lit terms, so include the emission here instead of the deferred
    // additive-emission pass used by ordinary lit emitters.
    if (material.isUnlit) {
        float3 emissive = SampleMaterialEmissive<StandardPBRMaterial>(material) * albedo.a;
        return float4(albedo.rgb + emissive, material.alphaMode == 2u ? albedo.a : 1.0f);
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
    // Forward shading uses the interpolated normal; its transmission branch
    // applies a separate derivative-based normal map before reaching here.
    material.geometricNormal = N;
    // The normal map is a material input, not a deferred-pass extra. The
    // tangent frame has to reach the material evaluation here exactly as it
    // does in the G-buffer path (basic.slang PSMain), or the shading normal of
    // every material the pipeline routes forward stays the geometric one and
    // the two paths disagree on every normal-mapped surface.
    material.hasTangent         = any(input.tangent.xyz);
    material.tangentDirection   = T;
    material.bitangentDirection = B;
    SurfaceInteraction surface = EvaluateMaterial<StandardPBRMaterial>(material, albedo, sampledEmissive);
    float roughness = surface.roughness;
    float metallic  = surface.metallic;
    float4 aniso = SampleMaterialAnisotropy(N, T, B, SceneTextureUV(input.uv, input.uv1, input.instanceId, kSamplerAnisotropy), input.anisotropyParams, input.anisotropyTexIndex, input.samplerCodes);
    float3 anisotropicT = aniso.z > 0.0f ? UnpackAnisotropy(aniso.xy, N) : T;
    float3 anisotropicB = cross(N, anisotropicT);
    // The shading normal is the normal-mapped one. The deferred path reads it
    // back from the G-buffer, so the forward path has to use the same normal
    // or the two paths disagree on every normal-mapped surface.
    float3 Ns     = surface.normal;
    float3 Vs     = SafeNormalize(scene.frame.camPos.xyz - worldPos, Ns);
    float  NdotVs = saturate(dot(Ns, Vs));
    float3 R      = AnisotropicReflection(Ns, Vs, anisotropicB, roughness, aniso.z);
    // KHR_materials_specular scales the dielectric reflectance and lowers the
    // grazing value; both reach the split-sum LUT's two coefficients.
    InstanceData inst   = scene.g_instances[input.instanceId];
    float3       baseF0 = float3(kDielectricF0);
    float        f90    = 1.0f;
    if (inst.hasSpecularResponse) {
        DielectricResponse response = ResolveDielectricResponse(inst, input.uv, input.uv1, input.instanceId);
        baseF0                      = response.f0;
        f90                         = response.f90;
    }
    float3 F0      = lerp(baseF0, albedo.rgb, metallic);
    float  f90Sum  = EffectiveF90(f90, metallic);
    float3 F_rough = SpecularFresnelRoughness(NdotVs, F0, f90Sum, roughness);
    float2 envBRDF = scene.brdfLUT.SampleLevel(scene.clampSampler, float2(NdotVs, roughness), 0.0f).rg;
    // The split-sum LUT already integrates Schlick's angle-dependent Fresnel.
    // Its A coefficient multiplies F0 and its B coefficient the grazing value,
    // not a second grazing-angle evaluation. Metals keep that value at 1.
    float3 FssEss   = F0 * envBRDF.x + float3(f90Sum) * envBRDF.y;
    float3 prefiltered = SampleAnisotropicEnvironment(scene.prefilteredMap, scene.clampSampler, R, roughness, aniso.z);
    float3 specularIBL = prefiltered * scene.frame.ambientExposure * FssEss;
    // The diffuse irradiance is the environment's own SH, evaluated exactly as
    // the deferred path evaluates it (lighting.slang: EvaluateSH(N, frame.sh) *
    // frame.ambientExposure). A constant 0.5 is a mid-grey placeholder: it
    // silently replaced the scenario's environment for every material the
    // pipeline routes forward, which is why those materials no longer match
    // the environment the goldens were lit with.
    float3 irradiance  = EvaluateSH(Ns, scene.frame.sh) * scene.frame.ambientExposure;
    float3 diffuseIBL  = albedo.rgb * (1.0f - F_rough) * (1.0f - metallic) * irradiance;
    float3 litBase = (diffuseIBL + specularIBL) * surface.occlusion;
    float3 sheenColor = surface.sheen.rgb;
    if (any(sheenColor > 0.0f)) {
        float attenuation = SheenAlbedoScaling(sheenColor, surface.sheen.a, NdotVs, NdotVs);
        float3 sheenEnv = scene.prefilteredMap.SampleLevel(scene.clampSampler, reflect(-Vs, Ns), max(surface.sheen.a, 0.2f) * 5.0f).rgb;
        litBase = litBase * attenuation + sheenEnv * scene.frame.ambientExposure *
                  SheenEnvironmentWeight(sheenColor, NdotVs) * surface.occlusion;
    }
    return float4(litBase + emissive, albedo.a);
}
