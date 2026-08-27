// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative GPU instance / meshlet / vertex-attribute ABI. common.slang
// imports this module and attaches methods; the host reflects the structs
// without loading the bindless ParameterBlock.
module instance_data;

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

    public float4 morphWeights;
    public float4 baseColorFactor;

    public float4 emissiveFactor;

    public uint64_t meshletAddress;
    public uint64_t meshletVertexAddress;
    public uint64_t meshletTriAddress;
    public uint meshletCount;
    public uint _paddingMeshlet;
};

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

// Behaviour-only accessors over the ABI above: no fields, padding, or
// descriptor bindings. They live here rather than in common.slang so that
// consumers of the raw ABI (ray-tracing helpers, standalone compute passes)
// get them without pulling in common's global `scene` ParameterBlock and
// push-constant declarations, which would change every binding table they
// are imported into.
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
        // Positions are a tightly packed host float3 stream (12-byte stride).
        // A float3* may be given a 16-byte array stride by SPIR-V layout rules.
        float* values    = (float*) this.posAddress;
        uint valueOffset = vertexId * 3u;
        return float3(values[valueOffset], values[valueOffset + 1u], values[valueOffset + 2u]);
    }

    [ForceInline]
    public VertexAttributes fetchAttributes(uint vertexId) {
        VertexAttributes* attributes = (VertexAttributes*) this.attrAddress;
        return attributes[vertexId];
    }

    // --- VK_EXT_mesh_shader accessors ---------------------------------------
    // Every meshlet stream is read through raw word loads for the same reason
    // fetchPosition/fetchSkin are: a typed pointer may be assigned a padded
    // array stride by SPIR-V layout rules, while the host writes tightly
    // packed data. 16 words == sizeof(GPUMeshlet).
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

    /// Global vertex index of local vertex `local` inside a meshlet.
    [ForceInline]
    public uint fetchMeshletVertex(uint vertexOffset, uint local) {
        uint* indices = (uint*) this.meshletVertexAddress;
        return indices[vertexOffset + local];
    }

    /// One micro-triangle (three meshlet-local 8-bit indices). The cooker pads
    /// every meshlet's triangle run to a 4-byte boundary, but a single triangle
    /// may still straddle a word, hence the per-byte extraction.
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
        // The host packs each influence record as three uints (12 bytes). Do
        // not index a SkinVertex* here: its GPU array stride can be rounded to
        // 16 bytes by the target layout, unlike the host's tightly packed BDA.
        uint* words     = (uint*) this.skinAddress;
        uint wordOffset = vertexId * 3u;
        SkinVertex result;
        result.joints  = uint2(words[wordOffset], words[wordOffset + 1u]);
        result.weights = words[wordOffset + 2u];
        return result;
    }
}
