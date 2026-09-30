// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module vertex_format;

// Match Zahlen/Vertex.hpp exactly: 8-byte animated tangent frame and
// 12-byte immutable UV/color stream. Both are indexed independently by vertex.
public struct VertexTangentFrame {
    public uint normal;
    public uint tangent;
};

public struct VertexSurface {
    public uint uv;
    public uint color;
    public uint uv1;
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

public extension VertexTangentFrame {
    public property float4 unpackedNormal {
        [ForceInline]
        get { return Unpack1010102(this.normal); }
    }
    public property float4 unpackedTangent {
        [ForceInline]
        get { return Unpack1010102(this.tangent); }
    }
}

public extension VertexSurface {
    public property float2 unpackedUV {
        [ForceInline]
        get { return UnpackHalf2(this.uv); }
    }
    public property float2 unpackedUV1 {
        [ForceInline]
        get { return UnpackHalf2(this.uv1); }
    }
    public property float4 unpackedColor {
        [ForceInline]
        get { return UnpackRGBA8(this.color); }
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
