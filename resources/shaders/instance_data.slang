// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module instance_data;

import vertex_format;
import cxx_abi;
import gpu_buffer;

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

    [CxxArray]
    public float4 morphWeights;
    [CxxArray]
    public float4 baseColorFactor;

    [CxxArray]
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
    public uint samplerCodes1; // Slots 8..14.

    // Linear-space sheen RGB and independent alpha-channel roughness. A zero
    // color disables the layer; these fields are not part of the film/coat union.
    [CxxArray]
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
    public float4 uvRow0[15];
    public float4 uvRow1[15];
};

[CxxSkip]
public struct GPUMeshlet {
    public uint vertexOffset;
    public uint triangleOffset;
    public uint vertexCount;
    public uint triangleCount;

    public float3 sphereCenter;
    public float sphereRadius;

    public float3 coneApex;
    public float3 coneAxis;
    public float coneCutoff;
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
    public property bool isViewmodel {
        [ForceInline]
        get {
            return ((this.flags >> 16) & 0xFFu) != 0;
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
        GPUBuffer<uint> words = GPUBuffer<uint>(this.meshletAddress);
        uint base = meshletIndex * 16u;

        GPUMeshlet m;
        m.vertexOffset   = words.load(base + 0u);
        m.triangleOffset = words.load(base + 1u);
        m.vertexCount    = words.load(base + 2u);
        m.triangleCount  = words.load(base + 3u);
        m.sphereCenter   = float3(asfloat(words.load(base + 4u)), asfloat(words.load(base + 5u)), asfloat(words.load(base + 6u)));
        m.sphereRadius   = asfloat(words.load(base + 7u));
        m.coneApex       = float3(asfloat(words.load(base + 8u)), asfloat(words.load(base + 9u)), asfloat(words.load(base + 10u)));
        m.coneAxis       = float3(asfloat(words.load(base + 11u)), asfloat(words.load(base + 12u)), asfloat(words.load(base + 13u)));
        m.coneCutoff     = asfloat(words.load(base + 14u));
        return m;
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
    public SkinVertex fetchSkin(uint vertexId) {
        GPUBuffer<uint> words = GPUBuffer<uint>(this.skinAddress);
        uint wordOffset = vertexId * 3u;
        SkinVertex result;
        result.joints  = uint2(words.load(wordOffset), words.load(wordOffset + 1u));
        result.weights = words.load(wordOffset + 2u);
        return result;
    }
}
