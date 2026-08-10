// resources/shaders/common_types.slang
// SPDX-License-Identifier: GPL-3.0-or-later
// Slang module: types and helpers split from common.hlsl for use when bindings should be excluded (modern replacement for #define SKIP_BINDINGS).
// Historically, culling.hlsl and punctual_shadows.hlsl used `#define SKIP_BINDINGS` + `#include "common.hlsl"` to get types without bindings.
// Modern Slang idiom: `import common_types;` instead.

module common_types;
import pbr_helpers;
import uniforms;

// --- Re-export InstanceData, GPUJoint, helper functions without bindings ---
// This file is generated as a thin wrapper; for full source see common.slang.
// It is included here to satisfy Slang's module system cleanly.

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
    public uint   _paddingCenter;

    public float4 morphWeights;
    public float4 baseColorFactor;
    public float4 emissiveFactor;
};

public struct ObjectConstants {
    public uint instanceId;
    public uint isShadowPass;
};

public struct GPUJoint {
    public float4 col0;
    public float4 col1;
    public float4 col2;
    public float4 col3;
};

// --- Attribute Unpackers ---
float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

float2 UnpackUV(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

float4 UnpackColor(uint packed) {
    return float4(
        float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f
    );
}

uint4 UnpackJoints(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

// Includes VSOutput, PSOutput, Skinning helpers etc. See common.slang for full definitions.
// For strict split, copy the post-binding helpers here without vk::binding declarations.

// End common_types
