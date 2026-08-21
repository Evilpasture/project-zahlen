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
    public uint _paddingCenter;

    public float4 morphWeights;
    public float4 baseColorFactor;

    public float4 emissiveFactor;

    // --- VK_EXT_mesh_shader meshlet streams (0 => no meshlets for this mesh) ---
    // Mirrors the tail of ZHLN::InstanceData (include/Zahlen/Types.hpp):
    //   offset 272: meshletAddress, 280: meshletVertexAddress,
    //   offset 288: meshletTriAddress, 296: meshletCount, 300: padding.
    public uint64_t meshletAddress;
    public uint64_t meshletVertexAddress;
    public uint64_t meshletTriAddress;
    public uint meshletCount;
    public uint _paddingMeshlet;
};

/// 64-byte meshlet descriptor. Kept in lockstep with ZHLN::GPUMeshlet.
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
    [ForceInline]
    public float4 transformPosition(float4 position) {
        return this.col0 * position.x + this.col1 * position.y + this.col2 * position.z + this.col3 * position.w;
    }

    [ForceInline]
    public float3 transformDirection(float3 direction) {
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
    public SamplerState defaultSampler;                // 0
    public ConstantBuffer<FrameUniforms> frame;        // 1
    public StructuredBuffer<Light> lights;             // 2
    public StructuredBuffer<InstanceData> g_instances; // 3
    public StructuredBuffer<GPUJoint> g_joints;        // 4
    public StructuredBuffer<GPUJoint> g_prevJoints;    // 5
    public StructuredBuffer<float4> g_morphDeltas;     // 6
    public TextureCube prefilteredMap;                 // 7
    public Texture2D brdfLUT;                          // 8
    public SamplerState clampSampler;                  // 9
    public Texture2D<float4> texTransLighting;         // 10
    public Texture2D globalTextures[];                 // 11
};

public ParameterBlock<GlobalSceneRegistry> scene;

[vk::push_constant]
public ObjectConstants obj;

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
    return float4(float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f,
                  float((packed >> 24) & 0xFF) / 255.0f);
}

public uint4 UnpackJoints(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

public extension VertexAttributes {
    public property float4 unpackedNormal {
        [ForceInline]
        get {
            return UnpackNormal(this.normal);
        }
    }
    public property float4 unpackedTangent {
        [ForceInline]
        get {
            return UnpackNormal(this.tangent);
        }
    }
    public property float2 unpackedUV {
        [ForceInline]
        get {
            return UnpackUV(this.uv);
        }
    }
    public property float4 unpackedColor {
        [ForceInline]
        get {
            return UnpackColor(this.color);
        }
    }
}

public extension SkinVertex {
    public property uint4 unpackedJoints {
        [ForceInline]
        get {
            return UnpackJoints(this.joints);
        }
    }
    public property float4 unpackedWeights {
        [ForceInline]
        get {
            return UnpackColor(this.weights);
        }
    }
}

// --- SKELETAL SKINNING ---
public float4 SkinPositionPrev(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = scene.g_prevJoints[jointOffset + joints.x];
    GPUJoint j1 = scene.g_prevJoints[jointOffset + joints.y];
    GPUJoint j2 = scene.g_prevJoints[jointOffset + joints.z];
    GPUJoint j3 = scene.g_prevJoints[jointOffset + joints.w];

    float4 pos = (j0.transformPosition(position)) * weights.x + (j1.transformPosition(position)) * weights.y + (j2.transformPosition(position)) * weights.z +
                 (j3.transformPosition(position)) * weights.w;
    return pos;
}

public float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset) {
    GPUJoint j0 = scene.g_joints[jointOffset + joints.x];
    GPUJoint j1 = scene.g_joints[jointOffset + joints.y];
    GPUJoint j2 = scene.g_joints[jointOffset + joints.z];
    GPUJoint j3 = scene.g_joints[jointOffset + joints.w];

    float4 pos = (j0.transformPosition(position)) * weights.x + (j1.transformPosition(position)) * weights.y + (j2.transformPosition(position)) * weights.z +
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

    float3 dir = (j0.transformDirection(direction)) * weights.x + (j1.transformDirection(direction)) * weights.y +
                 (j2.transformDirection(direction)) * weights.z + (j3.transformDirection(direction)) * weights.w;

    return dir;
}

public float3 GetMorphDisplacement(uint vertexId, uint vertexCount, uint morphOffset, uint activeMorphCount, float4 weights) {
    float3 displacement = float3(0, 0, 0);

    [unroll]
    for (uint i = 0; i < 4; ++i) {
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


// ============================================================================
// VK_EXT_mesh_shader cluster culling helpers
// ============================================================================

/// Extracts the six frustum planes from a view-projection matrix (mul(M, v)
/// convention, matching culling.slang) and tests a world-space sphere.
public bool SphereFrustumVisible(float4x4 viewProj, float3 center, float radius) {
    float4 r0 = float4(viewProj._m00, viewProj._m01, viewProj._m02, viewProj._m03);
    float4 r1 = float4(viewProj._m10, viewProj._m11, viewProj._m12, viewProj._m13);
    float4 r2 = float4(viewProj._m20, viewProj._m21, viewProj._m22, viewProj._m23);
    float4 r3 = float4(viewProj._m30, viewProj._m31, viewProj._m32, viewProj._m33);

    float4 planes[6];
    planes[0] = r3 + r0; // left
    planes[1] = r3 - r0; // right
    planes[2] = r3 + r1; // bottom
    planes[3] = r3 - r1; // top
    planes[4] = r2;      // near (zero-to-one depth)
    planes[5] = r3 - r2; // far

    [unroll]
    for (uint i = 0; i < 6; ++i) {
        float len = length(planes[i].xyz);
        planes[i] /= max(len, 1e-6f);
        if (dot(float4(center, 1.0f), planes[i]) < -radius) {
            return false;
        }
    }
    return true;
}

/// Largest scale factor of the upper-left 3x3 block; used to grow a local-space
/// bounding sphere into world space without decomposing the transform.
public float MatrixMaxScale(float4x4 m) {
    float3 c0 = float3(m._m00, m._m10, m._m20);
    float3 c1 = float3(m._m01, m._m11, m._m21);
    float3 c2 = float3(m._m02, m._m12, m._m22);
    return sqrt(max(dot(c0, c0), max(dot(c1, c1), dot(c2, c2))));
}

/// meshoptimizer normal-cone rejection. `coneCutoff` is cos(half angle) with the
/// meshoptimizer sign convention: the cluster is invisible when the view vector
/// lies inside the cone of back-facing normals.
///  * coneCutoff >= 1.0 marks a degenerate cone (never cull).
public bool ConeBackfaceCulled(float3 coneApexWorld, float3 coneAxisWorld, float coneCutoff, float3 cameraPos) {
    if (coneCutoff >= 1.0f) {
        return false;
    }
    float3 toCluster = coneApexWorld - cameraPos;
    float len        = length(toCluster);
    if (len < 1e-5f) {
        return false;
    }
    return dot(toCluster / len, coneAxisWorld) >= coneCutoff;
}

// ============================================================================
// Shared geometry-stage interface
// ============================================================================
//
// Declared here (rather than in basic.slang) so that the vertex path
// (basic.slang: VSMain) and the mesh path (basic_mesh.slang: MeshMain) emit a
// bit-identical interface to the same fragment shaders (PSMain / PSShadow /
// PSForward). Any divergence between the two would surface as a pipeline
// link error rather than as silently wrong shading.
//
// NOTE the deliberately qualified name: several unrelated shaders that import
// this module declare their own local `VSOutput`, and an exported symbol with
// that name would make every one of them ambiguous. basic.slang aliases it.
public struct SceneGeometryOutput {
    public float4 pos : SV_Position;
    public float4 currClip : TEXCOORD0;
    public float4 prevClip : TEXCOORD1;
    public float3 normal : NORMAL;
    public float4 tangent : TANGENT;
    public float2 uv : TEXCOORD2;
    public float4 color : COLOR;
    public nointerpolation uint4 materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;
    public nointerpolation uint alphaMode : TEXCOORD7;
    public nointerpolation float4 emissiveFactor : TEXCOORD8;
};

/// Payload handed from the task (amplification) stage to the mesh stage.
/// One entry per surviving meshlet in the workgroup.
public struct MeshletTaskPayload {
    public uint instanceId;
    public uint baseMeshlet;
    public uint meshletIndices[32];
};
