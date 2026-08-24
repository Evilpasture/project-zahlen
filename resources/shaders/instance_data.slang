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
