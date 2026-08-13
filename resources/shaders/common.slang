// resources/shaders/common.slang
module common;

import uniforms;

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

// These transient views describe the existing BDA element layouts. They are never
// reflected in InstanceData, so the C++ ABI remains unchanged.
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

// Extensions deliberately add behavior only: no fields, padding, or descriptor bindings.
public extension InstanceData {
    [ForceInline] public property uint albedoIndex { get { return this.texIndices0 & 0xFFFFu; } }
    [ForceInline] public property uint normalIndex { get { return this.texIndices0 >> 16; } }
    [ForceInline] public property uint pbrIndex { get { return this.texIndices1 & 0xFFFFu; } }
    [ForceInline] public property uint emissiveIndex { get { return this.texIndices1 >> 16; } }
    [ForceInline] public property uint alphaMode { get { return this.flags & 0xFFu; } }
    [ForceInline] public property bool isSkinned { get { return ((this.flags >> 8) & 0xFFu) != 0; } }
    [ForceInline] public property bool isViewmodel { get { return ((this.flags >> 16) & 0xFFu) != 0; } }

    [ForceInline] public uint fetchIndex(uint indexId) {
        if (this.iboAddress == 0) return indexId;
        uint* indices = (uint*)this.iboAddress;
        return indices[indexId];
    }

    [ForceInline] public float3 fetchPosition(uint vertexId) {
        if (this.posAddress == 0) return float3(0.0f);
        float3* positions = (float3*)this.posAddress;
        return positions[vertexId];
    }

    [ForceInline] public VertexAttributes fetchAttributes(uint vertexId) {
        VertexAttributes* attributes = (VertexAttributes*)this.attrAddress;
        return attributes[vertexId];
    }

    [ForceInline] public SkinVertex fetchSkin(uint vertexId) {
        SkinVertex* skin = (SkinVertex*)this.skinAddress;
        return skin[vertexId];
    }
}

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

public extension GPUJoint {
    [ForceInline] public float4 transformPosition(float4 position) {
        return this.col0 * position.x + this.col1 * position.y + this.col2 * position.z + this.col3 * position.w;
    }

    [ForceInline] public float3 transformDirection(float3 direction) {
        return this.col0.xyz * direction.x + this.col1.xyz * direction.y + this.col2.xyz * direction.z;
    }
}

/**
 * The authoritative layout of the global, per-frame bindless descriptor set.
 * The C++ side reflects the exact binding indices out of the compiled
 * shader modules; member order here dictates the binding numbers.
 * NOTE: `globalTextures` is a runtime array and therefore must stay LAST.
 */
public struct GlobalSceneRegistry {
    public SamplerState                   defaultSampler;   // 0
    public ConstantBuffer<FrameUniforms>  frame;            // 1
    public StructuredBuffer<Light>        lights;           // 2
    public StructuredBuffer<InstanceData> g_instances;      // 3
    public StructuredBuffer<GPUJoint>     g_joints;         // 4
    public StructuredBuffer<GPUJoint>     g_prevJoints;     // 5
    public StructuredBuffer<float4>       g_morphDeltas;    // 6
    public TextureCube                    prefilteredMap;   // 7
    public Texture2D                      brdfLUT;          // 8
    public SamplerState                   clampSampler;     // 9
    public Texture2D<float4>              texTransLighting; // 10
    public Texture2D                      globalTextures[]; // 11
};

public ParameterBlock<GlobalSceneRegistry> scene;

[vk::push_constant] public ObjectConstants obj;

// --- Attribute Unpackers ---
public float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

public float2 UnpackUV(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

public float4 UnpackColor(uint packed) {
    return float4(
        float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f
    );
}

public uint4 UnpackJoints(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

public extension VertexAttributes {
    [ForceInline] public property float4 unpackedNormal { get { return UnpackNormal(this.normal); } }
    [ForceInline] public property float4 unpackedTangent { get { return UnpackNormal(this.tangent); } }
    [ForceInline] public property float2 unpackedUV { get { return UnpackUV(this.uv); } }
    [ForceInline] public property float4 unpackedColor { get { return UnpackColor(this.color); } }
}

public extension SkinVertex {
    [ForceInline] public property uint4 unpackedJoints { get { return UnpackJoints(this.joints); } }
    [ForceInline] public property float4 unpackedWeights { get { return UnpackColor(this.weights); } }
}


// --- SKELETAL SKINNING ---
public float4 SkinPositionPrev(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = scene.g_prevJoints[jointOffset + joints.x];
    GPUJoint j1 = scene.g_prevJoints[jointOffset + joints.y];
    GPUJoint j2 = scene.g_prevJoints[jointOffset + joints.z];
    GPUJoint j3 = scene.g_prevJoints[jointOffset + joints.w];

    float4 pos = (j0.transformPosition(position)) * weights.x +
                 (j1.transformPosition(position)) * weights.y +
                 (j2.transformPosition(position)) * weights.z +
                 (j3.transformPosition(position)) * weights.w;
    return pos;
}

public float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = scene.g_joints[jointOffset + joints.x];
    GPUJoint j1 = scene.g_joints[jointOffset + joints.y];
    GPUJoint j2 = scene.g_joints[jointOffset + joints.z];
    GPUJoint j3 = scene.g_joints[jointOffset + joints.w];

    float4 pos = (j0.transformPosition(position)) * weights.x +
                 (j1.transformPosition(position)) * weights.y +
                 (j2.transformPosition(position)) * weights.z +
                 (j3.transformPosition(position)) * weights.w;
    return pos;
}

public float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset) {
    if (dot(weights, float4(1.5, 1.0, 1.0, 1.0)) < 0.001) {
        return direction;
    }

    GPUJoint j0 = scene.g_joints[jointOffset + joints.x];
    GPUJoint j1 = scene.g_joints[jointOffset + joints.y];
    GPUJoint j2 = scene.g_joints[jointOffset + joints.z];
    GPUJoint j3 = scene.g_joints[jointOffset + joints.w];

    float3 dir = (j0.transformDirection(direction)) * weights.x +
                 (j1.transformDirection(direction)) * weights.y +
                 (j2.transformDirection(direction)) * weights.z +
                 (j3.transformDirection(direction)) * weights.w;

    return dir;
}

public float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset, uint activeMorphCount, float4 weights) {
    float3 displacement = float3(0, 0, 0);

    [unroll] for (uint i = 0; i < 4; ++i) {
        if (i >= activeMorphCount) {
            break;
        }

        uint deltaIndex = morphOffset + (i * vertexCount) + vertexId;

        float weight = 0.0f;
        if (i == 0)
            weight = weights.x;
        else if (i == 1)
            weight = weights.y;
        else if (i == 2)
            weight = weights.z;
        else if (i == 3)
            weight = weights.w;

        displacement += scene.g_morphDeltas[deltaIndex].xyz * weight;
    }

    return displacement;
}
