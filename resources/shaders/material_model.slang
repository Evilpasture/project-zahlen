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
};

// Interpolants and material data shared by the raster passes. The tangent
// frame is prepared by the caller so the scene and particle vertex paths can
// retain their respective handedness/fallback conventions.
public struct MaterialSampleInputs {
    public float2 uv;
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
};

[ForceInline]
public MaterialSampleInputs MakeMaterialInputs(float2 uv, float4 color, uint4 indices, uint2 samplers,
                                                float4 baseFactor, float4 emissiveFactor, float3 pbrFactors, uint alphaMode) {
    MaterialSampleInputs i;
    i.uv                 = uv;
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
    return i;
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
        return SampleMaterialTexture(i.albedoIndex, i.uv, i.samplerCodes, kSamplerAlbedo) * i.baseColorFactor * i.vertexColor;
    }

    [ForceInline]
    public static float3 sampleEmissive(MaterialSampleInputs i) {
        return SampleMaterialTexture(i.emissiveIndex, i.uv, i.samplerCodes, kSamplerEmissive).rgb * i.emissiveFactor.rgb;
    }

    [ForceInline]
    public static SurfaceInteraction evaluate(MaterialSampleInputs i, float4 albedo, float3 emissive) {
        SurfaceInteraction s;
        s.albedo   = albedo.rgb;
        s.alpha    = albedo.a;
        s.emissive = emissive;

        float4 pbr  = SampleMaterialTexture(i.pbrIndex, i.uv, i.samplerCodes, kSamplerPbr);
        s.roughness = max((i.pbrIndex == 0 ? 1.0f : pbr.g) * i.roughnessFactor, i.roughnessFloor);
        s.metallic  = (i.pbrIndex == 0 ? 1.0f : pbr.b) * i.metallicFactor;
        s.occlusion = i.pbrIndex == 0 ? 1.0f : pbr.r;

        s.normal = i.geometricNormal;
        if (i.normalIndex != 2 && i.hasTangent) {
            float3 normalMap = SampleMaterialTexture(i.normalIndex, i.uv, i.samplerCodes, kSamplerNormal).rgb * 2.0f - 1.0f;
            s.normal = normalize(normalMap.x * i.tangentDirection + normalMap.y * i.bitangentDirection + normalMap.z * i.geometricNormal);
        }
        s.clearcoat  = float4(0.0f);
        s.anisotropy = float4(0.0f);
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
            coatFactor *= SampleMaterialTexture(i.coatTextures.x, i.uv, i.samplerCodes, kSamplerClearcoat).r;
        }
        if (coatFactor > 0.0f && i.coatTextures.y != 0xFFFFu) {
            coatRough *= SampleMaterialTexture(i.coatTextures.y, i.uv, i.samplerCodes, kSamplerClearcoatRoughness).g;
        }
        coatFactor = saturate(coatFactor);
        coatRough = saturate(coatRough);
        if (coatFactor > 1.0f / 255.0f && i.coatTextures.z != 0xFFFFu && i.hasTangent) {
            float3 mapN = SampleMaterialTexture(i.coatTextures.z, i.uv, i.samplerCodes, kSamplerClearcoatNormal).rgb * 2.0f - 1.0f;
            mapN.xy *= i.coatParams.z;
            coatNormal = normalize(mapN.x * i.tangentDirection + mapN.y * i.bitangentDirection + mapN.z * i.geometricNormal);
        }
        float2 packedCoat = PackNormalOctahedron(coatNormal) * 0.5f + 0.5f;
        s.clearcoat = coatFactor > 1.0f / 255.0f ? float4(packedCoat, coatRough, coatFactor) : float4(0.0f);
        s.anisotropy = SampleMaterialAnisotropy(s.normal, i.tangentDirection, i.bitangentDirection,
                                               i.uv, i.anisotropyParams, i.anisotropyTexIndex, i.samplerCodes);
        return s;
    }
};
