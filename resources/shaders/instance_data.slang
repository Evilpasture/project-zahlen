// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module instance_data;

import vertex_format;
import cxx_abi;

public struct InstanceData {
    public float4x4 world;
    public float4x4 prevWorld;
    public uint64_t posAddress;
    public uint64_t attrAddress;
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
    public uint _paddingAnisotropy;
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

public struct VertexAttributes {
    public uint normal;
    public uint tangent;
    public uint uv;
    public uint color;
};

public struct SkinVertex {
    public uint2 joints;
    public uint weights;
};

public extension VertexAttributes {
    public property float4 unpackedNormal {
        [ForceInline]
        get {
            return Unpack1010102(this.normal);
        }
    }
    public property float4 unpackedTangent {
        [ForceInline]
        get {
            return Unpack1010102(this.tangent);
        }
    }
    public property float2 unpackedUV {
        [ForceInline]
        get {
            return UnpackHalf2(this.uv);
        }
    }
    public property float4 unpackedColor {
        [ForceInline]
        get {
            return UnpackRGBA8(this.color);
        }
    }
}

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
            return ((this.flags >> 8) & 0xFFu) != 0;
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
        get {
            return float(this.flags >> 24) * (1.0f / 255.0f);
        }
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
    public property float filmThicknessMinNm {
        [ForceInline]
        get { return float(this._paddingMeshlet >> 16); }
    }
    [ForceInline]
    public float4 transmissionParams() {
        float t = this.transmissionFactor;
        if (t <= 0.0f) {
            return float4(0.0f, 0.0f, 0.0f, 0.0f);
        }
        return float4(t, this.emissiveFactor.w, this.alphaCutoff, this.filmThicknessMinNm);
    }
    [ForceInline]
    public float4 volumeParams() {
        if (this.transmissionFactor <= 0.0f) {
            return float4(0.0f, 1.5f, 1.0f, 1.0f);
        }
        return float4(this.baseColorFactor.a, this.metallicFactor, 1.0f, this.emissiveFactor.x);
    }
    [ForceInline]
    public uint4 transmissionTextures() {
        return uint4(this.filmThicknessTexIndex, this.iridescenceTexIndex, this.volumeThicknessTexIndex, 0u);
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
        uint* indices = (uint*) this.iboAddress;
        return indices[indexId];
    }

    [ForceInline]
    public float3 fetchPosition(uint vertexId) {
        if (this.posAddress == 0)
            return float3(0.0f);
        float* values    = (float*) this.posAddress;
        uint valueOffset = vertexId * 3u;
        return float3(values[valueOffset], values[valueOffset + 1u], values[valueOffset + 2u]);
    }

    [ForceInline]
    public VertexAttributes fetchAttributes(uint vertexId) {
        VertexAttributes* attributes = (VertexAttributes*) this.attrAddress;
        return attributes[vertexId];
    }

    [ForceInline]
    public bool hasMeshlets() {
        return this.meshletAddress != 0 && this.meshletVertexAddress != 0 && this.meshletTriAddress != 0 && this.meshletCount > 0;
    }

    [ForceInline]
    public GPUMeshlet fetchMeshlet(uint meshletIndex) {
        uint* words = (uint*) this.meshletAddress;
        uint base   = meshletIndex * 16u;

        GPUMeshlet m;
        m.vertexOffset   = words[base + 0u];
        m.triangleOffset = words[base + 1u];
        m.vertexCount    = words[base + 2u];
        m.triangleCount  = words[base + 3u];
        m.sphereCenter   = float3(asfloat(words[base + 4u]), asfloat(words[base + 5u]), asfloat(words[base + 6u]));
        m.sphereRadius   = asfloat(words[base + 7u]);
        m.coneApex       = float3(asfloat(words[base + 8u]), asfloat(words[base + 9u]), asfloat(words[base + 10u]));
        m.coneAxis       = float3(asfloat(words[base + 11u]), asfloat(words[base + 12u]), asfloat(words[base + 13u]));
        m.coneCutoff     = asfloat(words[base + 14u]);
        return m;
    }

    [ForceInline]
    public uint fetchMeshletVertex(uint vertexOffset, uint local) {
        uint* indices = (uint*) this.meshletVertexAddress;
        return indices[vertexOffset + local];
    }

    [ForceInline]
    public uint3 fetchMeshletTriangle(uint triangleOffset, uint triangleIndex) {
        uint* words   = (uint*) this.meshletTriAddress;
        uint byteBase = triangleOffset + (triangleIndex * 3u);

        uint b0 = (words[(byteBase + 0u) >> 2u] >> (((byteBase + 0u) & 3u) * 8u)) & 0xFFu;
        uint b1 = (words[(byteBase + 1u) >> 2u] >> (((byteBase + 1u) & 3u) * 8u)) & 0xFFu;
        uint b2 = (words[(byteBase + 2u) >> 2u] >> (((byteBase + 2u) & 3u) * 8u)) & 0xFFu;
        return uint3(b0, b1, b2);
    }

    [ForceInline]
    public SkinVertex fetchSkin(uint vertexId) {
        uint* words     = (uint*) this.skinAddress;
        uint wordOffset = vertexId * 3u;
        SkinVertex result;
        result.joints  = uint2(words[wordOffset], words[wordOffset + 1u]);
        result.weights = words[wordOffset + 2u];
        return result;
    }
}
