// resources/shaders/common.slang
module common;

import instance_data;
import uniforms;
import vertex_format;

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

// --- Attribute Unpackers (owned by vertex_format.slang) ---
public float4 UnpackNormal(uint packed) {
    return Unpack1010102(packed);
}

public float2 UnpackUV(uint packed) {
    return UnpackHalf2(packed);
}

public float4 UnpackColor(uint packed) {
    return UnpackRGBA8(packed);
}

public uint4 UnpackJoints(uint2 packed) {
    return UnpackJoints16(packed);
}

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

public extension SkinVertex {
    public property uint4 unpackedJoints {
        [ForceInline]
        get {
            return UnpackJoints16(this.joints);
        }
    }
    public property float4 unpackedWeights {
        [ForceInline]
        get {
            return UnpackRGBA8(this.weights);
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
// The full set of values the geometry stage computes. This is a plain value
// type, NOT an interface: it carries no semantics and is never rasterised, so
// VSMain/MeshMain can fill it unconditionally and stay free of #ifdefs.
public struct SceneGeometryVertex {
    public float4 pos;
    public float4 currClip;
    public float4 prevClip;
    public float3 normal;
    public float4 tangent;
    public float2 uv;
    public float4 color;
    public uint4 materialIndices;
    public float4 baseColorFactor;
    public float3 pbrFactors;
    public uint alphaMode;
    public float4 emissiveFactor;
};

// The actual varying interface, specialised per pass.
//
// A geometry stage output that no fragment stage reads is legal but wasteful,
// and the validation layer reports every one of them ("SPIR-V Interface ...
// no corresponding Input declared"). Since the three fragment shaders consume
// three different subsets, one shared interface cannot match them all -- so
// the interface itself is compiled per pass. The geometry stage and the
// fragment stage of a pipeline MUST be compiled with the same defines;
// Resource::GetSceneShaders() is what guarantees that pairing.
public struct SceneGeometryOutput {
    public float4 pos : SV_Position;

#if !defined(ZHLN_PASS_SHADOW) && !defined(FORWARD_PASS)
    // G-buffer only: motion vectors and the normal/tangent frame.
    public float4 currClip : TEXCOORD0;
    public float4 prevClip : TEXCOORD1;
    public float3 normal : NORMAL;
    public float4 tangent : TANGENT;
#endif

    public float2 uv : TEXCOORD2;
    public float4 color : COLOR;
    public nointerpolation uint4 materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;

#if !defined(FORWARD_PASS)
    // Alpha-mask testing: PSMain and PSShadow, but not PSForward.
    public nointerpolation uint alphaMode : TEXCOORD7;
#endif

#if !defined(ZHLN_PASS_SHADOW)
    // Emissive: PSMain and PSForward, but not the depth-only shadow pass.
    public nointerpolation float4 emissiveFactor : TEXCOORD8;
#endif
};

/// Narrows the full vertex payload down to the varyings this pass actually
/// rasterises. Stores to dropped members never happen, so the geometry stage
/// pays nothing for values the fragment stage would have discarded.
public SceneGeometryOutput PackGeometryOutput(SceneGeometryVertex v) {
    SceneGeometryOutput o;
    o.pos = v.pos;

#if !defined(ZHLN_PASS_SHADOW) && !defined(FORWARD_PASS)
    o.currClip = v.currClip;
    o.prevClip = v.prevClip;
    o.normal   = v.normal;
    o.tangent  = v.tangent;
#endif

    o.uv              = v.uv;
    o.color           = v.color;
    o.materialIndices = v.materialIndices;
    o.baseColorFactor = v.baseColorFactor;
    o.pbrFactors      = v.pbrFactors;

#if !defined(FORWARD_PASS)
    o.alphaMode = v.alphaMode;
#endif

#if !defined(ZHLN_PASS_SHADOW)
    o.emissiveFactor = v.emissiveFactor;
#endif

    return o;
}

/// Payload handed from the task (amplification) stage to the mesh stage.
/// One entry per surviving meshlet in the workgroup.
public struct MeshletTaskPayload {
    public uint instanceId;
    public uint baseMeshlet;
    public uint meshletIndices[32];
};
