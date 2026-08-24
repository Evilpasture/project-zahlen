// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Authoritative vertex attribute packing. The C++ cooker / importer write the
// same bit layout (Math::PackNormal / PackUV / PackColor); shaders only unpack.
module vertex_format;

public struct PackedVertexAttributes {
    public uint packedNormal;
    public uint packedTangent;
    public uint packedUV;
    public uint packedColor;
};

public struct PackedSkinVertex {
    public uint2 packedJoints;
    public uint packedWeights;
};

[ForceInline]
public float4 Unpack1010102(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

[ForceInline]
public float2 UnpackHalf2(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

[ForceInline]
public float4 UnpackRGBA8(uint packed) {
    return float4(float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f,
                  float((packed >> 24) & 0xFF) / 255.0f);
}

[ForceInline]
public uint4 UnpackJoints16(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

public extension PackedVertexAttributes {
    public property float4 normal {
        [ForceInline]
        get {
            return Unpack1010102(this.packedNormal);
        }
    }
    public property float4 tangent {
        [ForceInline]
        get {
            return Unpack1010102(this.packedTangent);
        }
    }
    public property float2 uv {
        [ForceInline]
        get {
            return UnpackHalf2(this.packedUV);
        }
    }
    public property float4 color {
        [ForceInline]
        get {
            return UnpackRGBA8(this.packedColor);
        }
    }
}

public extension PackedSkinVertex {
    public property uint4 joints {
        [ForceInline]
        get {
            return UnpackJoints16(this.packedJoints);
        }
    }
    public property float4 weights {
        [ForceInline]
        get {
            return UnpackRGBA8(this.packedWeights);
        }
    }
}
