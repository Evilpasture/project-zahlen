// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module instance_data;

import vertex_format;
import gpu_buffer;

// InstanceData.flags layout beyond the material byte (see BuildGPUInstanceData):
// bit 8 skinned, 9 double-sided, 10 transmission, 11 unlit, 12 mirrored,
// 13 background layer, 16 viewmodel.
//
// Bit 13 is METADATA ONLY. The 3D skybox is imported as ordinary geometry in
// the same world as the map: placed where the importer authored it, occluded
// and culled and lit like anything else. The flag stays because infrastructure
// still needs to know a surface is scenery rather than play space -- the TLAS
// instance mask (rays skip it, so it never lights the world) and the
// cascade-shadow filter (a 300 m backdrop must not shade the level).
public static const uint kInstanceFlagBackground = 1u << 13;
public static const uint kInstanceFlagAmbientCube = 1u << 20;

// Source's two-world depth remap used to live here: the main world compressed
// into [0, 0.95] and the skybox layer into [0.95, 1], so a background fragment
// could never win a depth test no matter where the miniscape sat. That split is
// retired -- the backdrop is ordinary geometry in the same world now and is
// ordered by real depth like everything else. The constant is kept at 1.0
// rather than deleted so every encode/decode site is an identity instead of
// quietly dividing by a stale split (or by zero).
public static const float kSkyDepthSplit = 1.0f;

public struct InstanceData {
    public float4x4 world;
    public float4x4 prevWorld;
    public uint64_t posAddress;
    public uint64_t tangentFrameAddress;
    public uint64_t surfaceAddress;
    public uint64_t skinAddress;
    public uint64_t iboAddress;

    public uint vertexCount;
    public uint indexCount;
    public uint texIndices0;
    public uint texIndices1;

    public float cullRadius;
    public float metallicFactor;
    public float roughnessFactor;
    public float alphaCutoff;

    public uint flags;
    public uint jointOffset;
    public uint morphOffset;
    public uint activeMorphCount;

    public float3 localCenter;
    public uint _paddingCenter;

    public float4 morphWeights;
    public float4 baseColorFactor;

    public float4 emissiveFactor;

    public uint64_t meshletAddress;
    public uint64_t meshletVertexAddress;
    public uint64_t meshletTriAddress;
    public uint meshletCount;
    public uint _paddingMeshlet;

    // Separate from the film/clearcoat union above: anisotropy can coexist
    // with either extension. The texture index is 0xFFFF when absent.
    public float anisotropyStrength;
    public float anisotropyRotation;
    public uint anisotropyTexIndex;
    public uint samplerCodes0; // Slots 0..7, four bits each; reuses anisotropy padding.
    public uint samplerCodes1; // Slots 8..15.

    // Linear-space sheen RGB and independent alpha-channel roughness. A zero
    // color disables the layer; these fields are not part of the film/coat union.
    public float4 sheenParams;
    public uint sheenColorTexIndex;
    public uint sheenRoughnessTexIndex;
    public uint occlusionTexIndex;
    public float occlusionStrength;

    // Transmission is independent of alpha coverage, metallicity and emission.
    // Do not alias those authored factors to store optical parameters: masked
    // glass still needs its baseColor alpha and cutoff in the forward shader.
    public float transmissionValue;
    public float transmissionIor;
    public float transmissionThicknessM;
    public float transmissionNormalScale;
    public float iridescenceFactor;
    public float filmThicknessNm;
    public float filmThicknessMinNm;
    public uint transmissionTexIndex; // 0xFFFF when absent; linear R channel.

    // One affine transform per textureInfo (not per shared image). Each row is
    // (u coefficient, v coefficient, offset, unused/UV set). Keep them in
    // InstanceData so vertex, fragment, meshlet and shadow paths agree.
    public float4 uvRow0[16];
    public float4 uvRow1[16];

    // KHR_materials_specular. Independent of the coat/film union above, and
    // resolved per-fragment rather than baked into albedo: the response scales
    // the dielectric F0 baseline and replaces its grazing value. Declared after
    // the UV rows so the float4 seats where both a tight and a 16-byte-aligned
    // layout put it, without an implicit pad either way.
    public float4 specularParams; // rgb: specularColorFactor; a: specularFactor.
    public uint specularTexIndex; // 0xFFFF when absent; slot 15's sampler code and UV row.
    public uint specularFlags;    // Bit 0: KHR extension; bits 1..2: texture mode; bits 3..4: deferred reflection flags.

    // Baked diffuse irradiance has its own bindless lane rather than stealing
    // the AO semantic or one of the sixteen glTF texture-info sampler slots.
    // It is sampled with VertexSurface.uv1 and the clamp sampler.
    public uint bakedIrradianceTexIndex; // 0xFFFF when absent.
    public float bakedIrradianceScale;

    // Ambient cube for instances without a lightmap, +X,-X,+Y,-Y,+Z,-Z linear
    // radiance. Read only when kInstanceFlagAmbientCube is set. Appended after
    // existing instance fields so their offsets remain stable.
    public float4 ambientCube[6];

    // Generic scale for the deferred image-based reflection response. Appended
    // after the ambient cube to preserve all existing field offsets.
    public float environmentReflectionFactor;
};

// Field order is load-bearing: coneCutoff's 4 bytes fill 44..48 so coneAxis
// seats at 48 with no implicit pad under any layout, which is what lets
// zshader generate the host side. Do not reorder.
public struct GPUMeshlet {
    public uint vertexOffset;
    public uint triangleOffset;
    public uint vertexCount;
    public uint triangleCount;

    public float3 sphereCenter;
    public float sphereRadius;

    public float3 coneApex;
    public float coneCutoff;
    public float3 coneAxis;
    public uint _padding;
};

public struct SkinVertex {
    public uint2 joints;
    public uint weights;
};

public float AverageWorldScale(float4x4 world) {
    float3x3 m = (float3x3) world;
    float sx = length(float3(m._m00, m._m10, m._m20));
    float sy = length(float3(m._m01, m._m11, m._m21));
    float sz = length(float3(m._m02, m._m12, m._m22));
    return max((sx + sy + sz) * (1.0f / 3.0f), 0.0001f);
}

public extension InstanceData {
    public property uint albedoIndex {
        [ForceInline]
        get {
            return this.texIndices0 & 0xFFFFu;
        }
    }
    public property uint normalIndex {
        [ForceInline]
        get {
            return this.texIndices0 >> 16;
        }
    }
    public property uint pbrIndex {
        [ForceInline]
        get {
            return this.texIndices1 & 0xFFFFu;
        }
    }
    public property uint emissiveIndex {
        [ForceInline]
        get {
            return this.texIndices1 >> 16;
        }
    }
    public property uint alphaMode {
        [ForceInline]
        get {
            return this.flags & 0xFFu;
        }
    }
    public property bool isSkinned {
        [ForceInline]
        get {
            return (this.flags & (1u << 8)) != 0;
        }
    }
    public property bool isDoubleSided {
        [ForceInline]
        get {
            return (this.flags & (1u << 9)) != 0;
        }
    }
    public property bool isUnlit {
        [ForceInline]
        get {
            return (this.flags & (1u << 11)) != 0;
        }
    }
    public property bool isMirrored {
        [ForceInline]
        get {
            return (this.flags & (1u << 12)) != 0;
        }
    }
    public property bool hasAmbientCube {
        [ForceInline]
        get {
            return (this.flags & kInstanceFlagAmbientCube) != 0;
        }
    }

    public property bool isViewmodel {
        [ForceInline]
        get {
            return (this.flags & (1u << 19)) != 0;
        }
    }
    public property float transmissionFactor {
        [ForceInline]
        get { return this.transmissionValue; }
    }
    public property uint filmThicknessTexIndex {
        [ForceInline]
        get { return this._paddingCenter & 0xFFFFu; }
    }
    public property uint volumeThicknessTexIndex {
        [ForceInline]
        get { return this._paddingCenter >> 16; }
    }
    public property uint iridescenceTexIndex {
        [ForceInline]
        get { return this._paddingMeshlet & 0xFFFFu; }
    }
    // True only for a material that declares KHR_materials_specular: the
    // extension's defaults reproduce the engine's own dielectric baseline, so
    // absence and an all-default extension must stay distinguishable.
    public property bool hasSpecularResponse {
        [ForceInline]
        get {
            return (this.specularFlags & 1u) != 0u;
        }
    }
    // 0: a shared image carries specularColorTexture.rgb and specularTexture.a,
    // 1: the bound image carries specularTexture.a only,
    // 2: the bound image carries specularColorTexture.rgb only.
    public property uint specularTextureMode {
        [ForceInline]
        get {
            return (this.specularFlags >> 1u) & 3u;
        }
    }
    // Optional per-material image-based reflection response, kept deferred so
    // ambient-cube and baked-light ownership remain intact.
    public property bool hasEnvironmentReflection {
        [ForceInline]
        get {
            return (this.specularFlags & (1u << 3u)) != 0u;
        }
    }
    public property bool environmentReflectionUsesAlphaMask {
        [ForceInline]
        get {
            return (this.specularFlags & (1u << 4u)) != 0u;
        }
    }
    public property bool hasBakedIrradiance {
        [ForceInline]
        get {
            return this.bakedIrradianceTexIndex != 0xFFFFu;
        }
    }
    // specularColorFactor (linear, clamped to [0, 1] by the importer).
    public property float3 specularColorFactor {
        [ForceInline]
        get {
            return this.specularParams.rgb;
        }
    }
    // specularFactor, the scale on F0 and the extension's grazing reflectance.
    // Unclamped above 1, which the extension permits and the forward paths use;
    // only the opaque G-buffer payload saturates it.
    public property float specularFactor {
        [ForceInline]
        get {
            return this.specularParams.a;
        }
    }

    [ForceInline]
    public float4 transmissionParams() {
        if (this.transmissionValue <= 0.0f) {
            return float4(0.0f);
        }
        return float4(this.transmissionValue, this.iridescenceFactor, this.filmThicknessNm, this.filmThicknessMinNm);
    }
    [ForceInline]
    public float4 volumeParams() {
        if (this.transmissionValue <= 0.0f) {
            return float4(0.0f, 1.5f, 1.0f, 1.0f);
        }
        return float4(this.transmissionThicknessM, this.transmissionIor, 1.0f, this.transmissionNormalScale);
    }
    [ForceInline]
    public uint4 transmissionTextures() {
        return uint4(this.filmThicknessTexIndex, this.iridescenceTexIndex, this.volumeThicknessTexIndex, this.transmissionTexIndex);
    }

    [ForceInline]
    public float clearcoatFactor() {
        if (this.transmissionFactor > 0.0f) {
            return 0.0f;
        }
        return float((this._paddingMeshlet >> 16) & 0xFFu) * (1.0f / 255.0f);
    }
    [ForceInline]
    public float clearcoatRoughnessFactor() {
        if (this.clearcoatFactor() <= 0.0f) {
            return 0.0f;
        }
        return this.emissiveFactor.w;
    }
    [ForceInline]
    public float clearcoatNormalScale() {
        return float(this._paddingMeshlet >> 24) * (1.0f / 64.0f);
    }
    [ForceInline]
    public uint clearcoatTexIndex() {
        return this._paddingCenter & 0xFFFFu;
    }
    [ForceInline]
    public uint clearcoatRoughnessTexIndex() {
        return this._paddingCenter >> 16;
    }
    [ForceInline]
    public uint clearcoatNormalTexIndex() {
        return this._paddingMeshlet & 0xFFFFu;
    }

    [ForceInline]
    public uint fetchIndex(uint indexId) {
        if (this.iboAddress == 0)
            return indexId;
        return GPUBuffer<uint>(this.iboAddress).load(indexId);
    }

    // Our vertex draws use sequential SV_VulkanVertexID and fetch the index
    // ourselves. Swap corners 1 and 2 for mirrored triangle lists so the
    // existing CCW raster front face, backface culling and SV_IsFrontFace
    // retain their meaning. Keep fetchIndex unchanged for BLAS/ray queries.
    [ForceInline]
    public uint fetchRasterIndex(uint indexId) {
        if (this.isMirrored) {
            uint corner = indexId % 3u;
            if (corner == 1u) indexId += 1u;
            if (corner == 2u) indexId -= 1u;
        }
        return this.fetchIndex(indexId);
    }

    [ForceInline]
    public float3 fetchPosition(uint vertexId) {
        if (this.posAddress == 0)
            return float3(0.0f);
        GPUBuffer<float> values = GPUBuffer<float>(this.posAddress);
        uint valueOffset = vertexId * 3u;
        return float3(values.load(valueOffset), values.load(valueOffset + 1u), values.load(valueOffset + 2u));
    }

    [ForceInline]
    public VertexTangentFrame fetchTangentFrame(uint vertexId) {
        return GPUBuffer<VertexTangentFrame>(this.tangentFrameAddress).load(vertexId);
    }

    [ForceInline]
    public VertexSurface fetchSurface(uint vertexId) {
        return GPUBuffer<VertexSurface>(this.surfaceAddress).load(vertexId);
    }

    [ForceInline]
    public bool hasMeshlets() {
        return this.meshletAddress != 0 && this.meshletVertexAddress != 0 && this.meshletTriAddress != 0 && this.meshletCount > 0;
    }

    [ForceInline]
    public GPUMeshlet fetchMeshlet(uint meshletIndex) {
        return GPUBuffer<GPUMeshlet>(this.meshletAddress).load(meshletIndex);
    }

    [ForceInline]
    public uint fetchMeshletVertex(uint vertexOffset, uint local) {
        return GPUBuffer<uint>(this.meshletVertexAddress).load(vertexOffset + local);
    }

    [ForceInline]
    public uint3 fetchMeshletTriangle(uint triangleOffset, uint triangleIndex) {
        GPUBuffer<uint> words = GPUBuffer<uint>(this.meshletTriAddress);
        uint byteBase = triangleOffset + (triangleIndex * 3u);

        uint b0 = (words.load((byteBase + 0u) >> 2u) >> (((byteBase + 0u) & 3u) * 8u)) & 0xFFu;
        uint b1 = (words.load((byteBase + 1u) >> 2u) >> (((byteBase + 1u) & 3u) * 8u)) & 0xFFu;
        uint b2 = (words.load((byteBase + 2u) >> 2u) >> (((byteBase + 2u) & 3u) * 8u)) & 0xFFu;
        return uint3(b0, b1, b2);
    }

    [ForceInline]
    public uint3 fetchRasterMeshletTriangle(uint triangleOffset, uint triangleIndex) {
        uint3 tri = this.fetchMeshletTriangle(triangleOffset, triangleIndex);
        return this.isMirrored ? tri.xzy : tri;
    }

    [ForceInline]
    public SkinVertex fetchSkin(uint vertexId) {
        GPUBuffer<uint> words = GPUBuffer<uint>(this.skinAddress);
        uint wordOffset = vertexId * 3u;
        SkinVertex result;
        result.joints  = uint2(words.load(wordOffset), words.load(wordOffset + 1u));
        result.weights = words.load(wordOffset + 2u);
        return result;
    }
}
