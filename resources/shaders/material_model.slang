// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Material decoding + surface evaluation. Geometry stages only provide UVs,
// tangent frames, and bindless indices; this module owns channel unpacking
// and the metallic-roughness surface model.
module material_model;

import common;
import pbr_helpers;

public struct SurfaceInteraction {
    public float3 albedo;
    public float3 normal;
    public float roughness;
    public float metallic;
    public float3 emissive;
    public float occlusion;
    public float alpha;
};

public struct MaterialSampleInputs {
    public float2 uv;
    public float4 vertexColor;
    public uint albedoIndex;
    public uint normalIndex;
    public uint pbrIndex;
    public uint emissiveIndex;
    public float4 baseColorFactor;
    public float4 emissiveFactor;
    public float metallicFactor;
    public float roughnessFactor;
    public float alphaCutoff;
    public uint alphaMode;
    public float3 geometricNormal;
    public float4 tangent;
};

public interface IMaterial {
    SurfaceInteraction evaluate(MaterialSampleInputs i);
}

public struct StandardPBRMaterial : IMaterial {
    public SurfaceInteraction evaluate(MaterialSampleInputs i) {
        SurfaceInteraction s;
        float4 albedoSample = scene.globalTextures[i.albedoIndex].Sample(scene.defaultSampler, i.uv) * i.baseColorFactor * i.vertexColor;
        s.albedo            = albedoSample.rgb;
        s.alpha             = albedoSample.a;
        s.emissive          = scene.globalTextures[i.emissiveIndex].Sample(scene.defaultSampler, i.uv).rgb * i.emissiveFactor.rgb;

        float4 pbr  = scene.globalTextures[i.pbrIndex].Sample(scene.defaultSampler, i.uv);
        s.roughness = max((i.pbrIndex == 0 ? 1.0f : pbr.g) * i.roughnessFactor, 0.045f);
        s.metallic  = (i.pbrIndex == 0 ? 1.0f : pbr.b) * i.metallicFactor;
        s.occlusion = i.pbrIndex == 0 ? 1.0f : pbr.r;

        if (i.alphaMode == 1) {
            s.roughness = 0.0f;
        }

        float3 N           = normalize(i.geometricNormal);
        float3 worldNormal = N;
        if (i.normalIndex != 2 && any(i.tangent.xyz)) {
            float3 T_unnorm = i.tangent.xyz - dot(i.tangent.xyz, N) * N;
            if (dot(T_unnorm, T_unnorm) < 0.0001f) {
                T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
            }
            float3 T          = normalize(T_unnorm);
            float tangentSign = i.tangent.w;
            float3 B          = normalize(cross(N, T) * tangentSign);
            float3 normalMap  = scene.globalTextures[i.normalIndex].Sample(scene.defaultSampler, i.uv).rgb * 2.0f - 1.0f;
            worldNormal       = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
        }
        s.normal = worldNormal;
        return s;
    }
}

public struct ClearCoatMaterial : IMaterial {
    public float clearCoat;
    public float clearCoatRoughness;

    public SurfaceInteraction evaluate(MaterialSampleInputs i) {
        StandardPBRMaterial base;
        SurfaceInteraction s = base.evaluate(i);
        s.roughness          = lerp(s.roughness, this.clearCoatRoughness, this.clearCoat);
        return s;
    }
}
