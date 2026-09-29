// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module material_model;

import common;
import pbr_helpers;

// A shader-local result, not a descriptor or a host-visible material ABI.
public struct SurfaceInteraction {
    public float3 albedo;
    public float3 normal;
    public float roughness;
    public float metallic;
    public float3 emissive;
    public float occlusion;
    public float alpha;
    public float4 clearcoat;
    public float4 anisotropy;
    public float4 sheen; // linear RGB and independent roughness
};

// Interpolants and material data shared by the raster passes. The tangent
// frame is prepared by the caller so the scene and particle vertex paths can
// retain their respective handedness/fallback conventions.
public struct MaterialSampleInputs {
    public float2 uv;
    public float2 uv1;
    public uint instanceId;
    public float4 vertexColor;
    public uint albedoIndex;
    public uint normalIndex;
    public uint pbrIndex;
    public uint emissiveIndex;
    public uint2 samplerCodes;
    public float4 baseColorFactor;
    public float4 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float alphaCutoff;
    public uint alphaMode;
    public float roughnessFloor;
    public float3 geometricNormal;
    public float3 tangentDirection;
    public float3 bitangentDirection;
    public bool hasTangent;
    public float4 coatParams;
    public uint4 coatTextures;
    public float2 anisotropyParams;
    public uint anisotropyTexIndex;
    public float4 sheenParams;
    public uint sheenColorTexIndex;
    public uint sheenRoughnessTexIndex;
    public uint occlusionTexIndex;
    public float occlusionStrength;
};

[ForceInline]
public MaterialSampleInputs MakeMaterialInputs(float2 uv, float4 color, uint4 indices, uint2 samplers,
                                                float4 baseFactor, float4 emissiveFactor, float3 pbrFactors, uint alphaMode) {
    MaterialSampleInputs i;
    i.uv                 = uv;
    i.uv1                = uv;
    i.instanceId         = 0xFFFFFFFFu; // Particle paths without an InstanceData UV transform.
    i.vertexColor        = color;
    i.albedoIndex        = indices.x;
    i.normalIndex        = indices.y;
    i.pbrIndex           = indices.z;
    i.emissiveIndex      = indices.w;
    i.samplerCodes       = samplers;
    i.baseColorFactor    = baseFactor;
    i.emissiveFactor     = emissiveFactor;
    i.metallicFactor     = pbrFactors.x;
    i.roughnessFactor    = pbrFactors.y;
    i.alphaCutoff        = pbrFactors.z;
    i.alphaMode          = alphaMode;
    i.roughnessFloor     = 0.001f;
    i.geometricNormal    = float3(0.0f, 1.0f, 0.0f);
    i.tangentDirection   = float3(1.0f, 0.0f, 0.0f);
    i.bitangentDirection = float3(0.0f, 0.0f, 1.0f);
    i.hasTangent         = false;
    i.coatParams         = float4(0.0f);
    i.coatTextures       = uint4(0xFFFFu);
    i.anisotropyParams   = float2(0.0f);
    i.anisotropyTexIndex = 0xFFFFu;
    i.sheenParams        = float4(0.0f);
    i.sheenColorTexIndex = 0xFFFFu;
    i.sheenRoughnessTexIndex = 0xFFFFu;
    i.occlusionTexIndex  = 0xFFFFu;
    i.occlusionStrength  = 1.0f;
    return i;
}

// The extension is defined on each textureInfo, not on the mesh or the
// texture object. The instance index is flat across the primitive; fetching
// its rows in the fragment preserves correct derivatives for minification.
[ForceInline]
public float2 SceneTextureUV(float2 uv0, float2 uv1, uint instanceId, uint slot) {
    if (instanceId == 0xFFFFFFFFu) return uv0;
    float4 row0 = scene.g_instances[instanceId].uvRow0[slot];
    float4 row1 = scene.g_instances[instanceId].uvRow1[slot];
    float2 uv = row1.w == 1.0f ? uv1 : row1.w == 0.0f ? uv0 : float2(0.0f);
    return float2(dot(row0.xy, uv) + row0.z, dot(row1.xy, uv) + row1.z);
}

[ForceInline]
public float2 MaterialTextureUV(MaterialSampleInputs i, uint slot) {
    return SceneTextureUV(i.uv, i.uv1, i.instanceId, slot);
}

// Tangent-space normal directions must follow a rotated or mirrored normal
// texture. Remove scale magnitude (it changes frequency, not bump height).
[ForceInline]
public float3 TransformNormalSample(float3 mapN, MaterialSampleInputs i, uint slot) {
    if (i.instanceId == 0xFFFFFFFFu) return mapN;
    float4 row0 = scene.g_instances[i.instanceId].uvRow0[slot];
    float4 row1 = scene.g_instances[i.instanceId].uvRow1[slot];
    float2 x = float2(row0.x, row1.x);
    float2 y = float2(row0.y, row1.y);
    return float3(dot(mapN.xy, x) / max(length(x), 1e-6f),
                  dot(mapN.xy, y) / max(length(y), 1e-6f), mapN.z);
}

[ForceInline]
public void ConfigureSceneMaterial(inout MaterialSampleInputs i, float2 uv1, uint instanceId) {
    i.instanceId = instanceId;
    i.uv1 = uv1;
    InstanceData inst = scene.g_instances[instanceId];
    i.sheenParams = inst.sheenParams;
    i.sheenColorTexIndex = inst.sheenColorTexIndex;
    i.sheenRoughnessTexIndex = inst.sheenRoughnessTexIndex;
    i.occlusionTexIndex = inst.occlusionTexIndex;
    i.occlusionStrength = inst.occlusionStrength;
}

// Staging these operations keeps alpha-only shadow passes from sampling PBR
// and lets forward's full-bright/transmission paths exit before evaluation.
// Slang specializes these calls for M at compile time; no GPU-side vtable or
// per-pixel material-type switch is introduced.
public interface IMaterial {
    static float4 sampleAlbedo(MaterialSampleInputs i);
    static float3 sampleEmissive(MaterialSampleInputs i);
    static SurfaceInteraction evaluate(MaterialSampleInputs i, float4 albedo, float3 emissive);
}

[ForceInline]
public float4 SampleMaterialAlbedo<M : IMaterial>(MaterialSampleInputs i) {
    return M.sampleAlbedo(i);
}

[ForceInline]
public float3 SampleMaterialEmissive<M : IMaterial>(MaterialSampleInputs i) {
    return M.sampleEmissive(i);
}

[ForceInline]
public SurfaceInteraction EvaluateMaterial<M : IMaterial>(MaterialSampleInputs i, float4 albedo, float3 emissive) {
    return M.evaluate(i, albedo, emissive);
}

[ForceInline]
public bool RejectMaskedAlpha(MaterialSampleInputs i, float alpha) {
    return i.alphaMode == 1 && alpha < i.alphaCutoff;
}

public struct StandardPBRMaterial : IMaterial {
    [ForceInline]
    public static float4 sampleAlbedo(MaterialSampleInputs i) {
        return SampleMaterialTexture(i.albedoIndex, MaterialTextureUV(i, kSamplerAlbedo), i.samplerCodes, kSamplerAlbedo) * i.baseColorFactor * i.vertexColor;
    }

    [ForceInline]
    public static float3 sampleEmissive(MaterialSampleInputs i) {
        return SampleMaterialTexture(i.emissiveIndex, MaterialTextureUV(i, kSamplerEmissive), i.samplerCodes, kSamplerEmissive).rgb * i.emissiveFactor.rgb;
    }

    [ForceInline]
    public static SurfaceInteraction evaluate(MaterialSampleInputs i, float4 albedo, float3 emissive) {
        SurfaceInteraction s;
        s.albedo   = albedo.rgb;
        s.alpha    = albedo.a;
        s.emissive = emissive;

        float4 pbr  = SampleMaterialTexture(i.pbrIndex, MaterialTextureUV(i, kSamplerPbr), i.samplerCodes, kSamplerPbr);
        s.roughness = max((i.pbrIndex == 0 ? 1.0f : pbr.g) * i.roughnessFactor, i.roughnessFloor);
        s.metallic  = (i.pbrIndex == 0 ? 1.0f : pbr.b) * i.metallicFactor;
        s.occlusion = 1.0f;
        if (i.occlusionTexIndex != 0xFFFFu) {
            float texel = SampleMaterialTexture(i.occlusionTexIndex, MaterialTextureUV(i, kSamplerOcclusion),
                                                 i.samplerCodes, kSamplerOcclusion).r;
            s.occlusion = lerp(1.0f, texel, saturate(i.occlusionStrength));
        }

        s.normal = i.geometricNormal;
        if (i.normalIndex != 2 && i.hasTangent) {
            float3 normalMap = SampleMaterialTexture(i.normalIndex, MaterialTextureUV(i, kSamplerNormal), i.samplerCodes, kSamplerNormal).rgb * 2.0f - 1.0f;
            normalMap = TransformNormalSample(normalMap, i, kSamplerNormal);
            s.normal = normalize(normalMap.x * i.tangentDirection + normalMap.y * i.bitangentDirection + normalMap.z * i.geometricNormal);
        }
        s.clearcoat  = float4(0.0f);
        s.anisotropy = float4(0.0f, 0.0f, 0.0f, s.occlusion);
        float3 sheenColor = saturate(i.sheenParams.rgb);
        float sheenRoughness = saturate(i.sheenParams.a);
        if (any(sheenColor > 0.0f)) {
            if (i.sheenColorTexIndex != 0xFFFFu) {
                // The bound image is VK_FORMAT_*_SRGB; sampling returns linear RGB.
                sheenColor *= SampleMaterialTexture(i.sheenColorTexIndex, MaterialTextureUV(i, kSamplerSheenColor),
                                                    i.samplerCodes, kSamplerSheenColor).rgb;
            }
            if (i.sheenRoughnessTexIndex != 0xFFFFu) {
                sheenRoughness *= SampleMaterialTexture(i.sheenRoughnessTexIndex, MaterialTextureUV(i, kSamplerSheenRoughness),
                                                       i.samplerCodes, kSamplerSheenRoughness).a;
            }
        }
        s.sheen = float4(sheenColor, sheenRoughness);
        return s;
    }
};

// glTF's optional RG channels are a direction in tangent space and B masks
// strength. Rotation is in radians; the no-texture direction is (1, 0).
public float4 SampleMaterialAnisotropy(float3 N, float3 T, float3 B, float2 uv, float2 params, uint texIndex, uint2 samplerCodes) {
    float strength = saturate(params.x);
    if (strength <= 0.0f) {
        return float4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    float2 direction = float2(1.0f, 0.0f);
    if (texIndex != 0xFFFFu) {
        float3 sample = SampleMaterialTexture(texIndex, uv, samplerCodes, kSamplerAnisotropy).rgb;
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

// The clearcoat is a separate layer, not a blend of base and coat roughness:
// the G-buffer stores its normal, roughness and strength independently.
// Anisotropy can coexist with the coat and is packed into its own attachment.
public struct ClearCoatMaterial : IMaterial {
    [ForceInline]
    public static float4 sampleAlbedo(MaterialSampleInputs i) {
        return StandardPBRMaterial.sampleAlbedo(i);
    }

    [ForceInline]
    public static float3 sampleEmissive(MaterialSampleInputs i) {
        return StandardPBRMaterial.sampleEmissive(i);
    }

    [ForceInline]
    public static SurfaceInteraction evaluate(MaterialSampleInputs i, float4 albedo, float3 emissive) {
        SurfaceInteraction s = StandardPBRMaterial.evaluate(i, albedo, emissive);
        float3 coatNormal = i.geometricNormal;
        float coatFactor = i.coatParams.x;
        float coatRough = i.coatParams.y;
        if (coatFactor > 0.0f && i.coatTextures.x != 0xFFFFu) {
            coatFactor *= SampleMaterialTexture(i.coatTextures.x, MaterialTextureUV(i, kSamplerClearcoat), i.samplerCodes, kSamplerClearcoat).r;
        }
        if (coatFactor > 0.0f && i.coatTextures.y != 0xFFFFu) {
            coatRough *= SampleMaterialTexture(i.coatTextures.y, MaterialTextureUV(i, kSamplerClearcoatRoughness), i.samplerCodes, kSamplerClearcoatRoughness).g;
        }
        coatFactor = saturate(coatFactor);
        coatRough = saturate(coatRough);
        if (coatFactor > 1.0f / 255.0f && i.coatTextures.z != 0xFFFFu && i.hasTangent) {
            float3 mapN = SampleMaterialTexture(i.coatTextures.z, MaterialTextureUV(i, kSamplerClearcoatNormal), i.samplerCodes, kSamplerClearcoatNormal).rgb * 2.0f - 1.0f;
            mapN.xy *= i.coatParams.z;
            mapN = TransformNormalSample(mapN, i, kSamplerClearcoatNormal);
            coatNormal = normalize(mapN.x * i.tangentDirection + mapN.y * i.bitangentDirection + mapN.z * i.geometricNormal);
        }
        float2 packedCoat = PackNormalOctahedron(coatNormal) * 0.5f + 0.5f;
        s.clearcoat = coatFactor > 1.0f / 255.0f ? float4(packedCoat, coatRough, coatFactor) : float4(0.0f);
        s.anisotropy = SampleMaterialAnisotropy(s.normal, i.tangentDirection, i.bitangentDirection,
                                               MaterialTextureUV(i, kSamplerAnisotropy), i.anisotropyParams, i.anisotropyTexIndex, i.samplerCodes);
        s.anisotropy.w = s.occlusion;
        return s;
    }
};
