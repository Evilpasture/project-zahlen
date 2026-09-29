module common;

__exported import instance_data;
__exported import uniforms;
__exported import vertex_format;


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

public struct GlobalSceneRegistry {
    public SamplerState defaultSampler;
    public ConstantBuffer<FrameUniforms> frame;
    public StructuredBuffer<Light> lights;
    public StructuredBuffer<InstanceData> g_instances;
    public StructuredBuffer<GPUJoint> g_joints;
    public StructuredBuffer<GPUJoint> g_prevJoints;
    public StructuredBuffer<float4> g_morphDeltas;
    public TextureCube prefilteredMap;
    public Texture2D brdfLUT;
    public SamplerState clampSampler;
    public Texture2D<float4> texTransLighting;
    public SamplerState materialSamplers[9]; // Three wrap modes on S x three on T.
    public Texture2D globalTextures[];
};

public ParameterBlock<GlobalSceneRegistry> scene;

// MaterialTextureSlot in Render/Types.hpp uses this exact order. The first
// eight four-bit sampler codes are packed in x and the remaining six in y.
public static const uint kSamplerAlbedo             = 0u;
public static const uint kSamplerNormal             = 1u;
public static const uint kSamplerPbr                = 2u;
public static const uint kSamplerEmissive           = 3u;
public static const uint kSamplerClearcoat          = 4u;
public static const uint kSamplerClearcoatRoughness = 5u;
public static const uint kSamplerClearcoatNormal    = 6u;
public static const uint kSamplerAnisotropy         = 7u;
public static const uint kSamplerIridescence        = 8u;
public static const uint kSamplerFilmThickness      = 9u;
public static const uint kSamplerVolumeThickness    = 10u;
public static const uint kSamplerSheenColor         = 11u;
public static const uint kSamplerSheenRoughness     = 12u;
public static const uint kSamplerOcclusion          = 13u;

[ForceInline]
public float4 SampleMaterialTexture(uint index, float2 uv, uint2 codes, uint slot) {
    uint packed = slot < 8u ? codes.x : codes.y;
    uint code = (packed >> ((slot & 7u) * 4u)) & 0xFu;
    // An absent sampler encodes Repeat/Repeat (0). Both image and sampler
    // indices may differ across materials in the same wavefront.
    return scene.globalTextures[NonUniformResourceIndex(index)]
        .Sample(scene.materialSamplers[NonUniformResourceIndex(min(code, 8u))], uv);
}

[vk::push_constant]
public ObjectConstants obj;

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



public bool SphereFrustumVisible(float4x4 viewProj, float3 center, float radius) {
    float4 r0 = float4(viewProj._m00, viewProj._m01, viewProj._m02, viewProj._m03);
    float4 r1 = float4(viewProj._m10, viewProj._m11, viewProj._m12, viewProj._m13);
    float4 r2 = float4(viewProj._m20, viewProj._m21, viewProj._m22, viewProj._m23);
    float4 r3 = float4(viewProj._m30, viewProj._m31, viewProj._m32, viewProj._m33);

    float4 planes[6];
    planes[0] = r3 + r0;
    planes[1] = r3 - r0;
    planes[2] = r3 + r1;
    planes[3] = r3 - r1;
    planes[4] = r2;
    planes[5] = r3 - r2;

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

public float MatrixMaxScale(float4x4 m) {
    float3 c0 = float3(m._m00, m._m10, m._m20);
    float3 c1 = float3(m._m01, m._m11, m._m21);
    float3 c2 = float3(m._m02, m._m12, m._m22);
    return sqrt(max(dot(c0, c0), max(dot(c1, c1), dot(c2, c2))));
}

// Cone backface test over the meshlet's bounding sphere.
//
// The apex formulation this replaced is gone rather than left alongside: it
// tested `normalize(coneApex - cameraPos)` against the cone axis, which flips
// as the camera crosses the plane through the apex -- a plane that for a
// close-up sits between the camera and the surface -- so it culled front-facing
// meshlets at exactly the distances where a meshlet being dropped is most
// visible. The only remaining reference to it anywhere was a CPU test that
// re-implemented both formulations and asserted on the copy, so deleting the
// shader function deleted the last thing keeping the trap load-bearing.
//
// The sphere form has no such plane: `dot(toCenter, axis)` is compared against
// `cutoff * dist + radius`, which is the exact condition for the camera to be
// outside the cone's tangent planes, and it degrades to "do not cull" as the
// camera approaches the bounding sphere instead of flipping.
public bool ConeBackfaceCulledSphere(float3 sphereCenterWorld, float radiusWorld, float3 coneAxisWorld, float coneCutoff, float3 cameraPos) {
    if (coneCutoff >= 1.0f) {
        return false;
    }
    float3 toCenter = sphereCenterWorld - cameraPos;
    float dist      = length(toCenter);
    if (dist < 1e-5f) {
        return false;
    }
    return dot(toCenter, coneAxisWorld) >= coneCutoff * dist + radiusWorld;
}

public struct SceneGeometryVertex {
    public float4 pos;
    public float4 currClip;
    public float4 prevClip;
    public float3 normal;
    public float4 tangent;
    public float2 uv;
    public float2 uv1;
    public uint instanceId;
    public float4 color;
    public uint4 materialIndices;
    public float4 baseColorFactor;
    public float3 pbrFactors;
    public uint alphaMode;
    public float4 emissiveFactor;
    public float4 transmissionParams;
    public float4 volumeParams;
    public uint4 transmissionTextures;
    public float4 coatParams;
    public uint4 coatTextures;
    public float2 anisotropyParams; // Strength, rotation (radians).
    public uint anisotropyTexIndex;
    public uint2 samplerCodes;
};

// Both indexed draws and meshlet draws supply their own vertex ID. Everything
// after that lookup -- attribute defaults, morphing, skinning and material
// payload -- is the same. Callers select SV_Position for their pass; motion
// clips are computed here only when requested.
public extension InstanceData {
    [ForceInline]
    public SceneGeometryVertex EvaluateVertex<bool withMotion>(uint actualVertexId, out float4 worldPos) {
        actualVertexId = min(actualVertexId, max(this.vertexCount, 1u) - 1u);
        SceneGeometryVertex output;

        float3 position     = this.fetchPosition(actualVertexId);
        float2 localUV      = float2(0, 0);
        float2 localUV1     = float2(0, 0);
        float4 localColor   = float4(1, 1, 1, 1);
        float3 localNormal  = float3(0, 1, 0);
        float4 localTangent = float4(1, 0, 0, 1);

        if (this.attrAddress != 0) {
            VertexAttributes attributes = this.fetchAttributes(actualVertexId);
            localNormal                 = attributes.unpackedNormal.xyz;
            localTangent                = attributes.unpackedTangent;
            localUV                     = attributes.unpackedUV;
            localUV1                    = attributes.unpackedUV1;
            localColor                  = attributes.unpackedColor;
        }

        float4 localPos = float4(position, 1.0f);

        uint4 localJoints = uint4(0, 0, 0, 0);
        float4 weights    = float4(0.0f, 0.0f, 0.0f, 0.0f);

        if (this.isSkinned && this.skinAddress != 0) {
            SkinVertex skin = this.fetchSkin(actualVertexId);
            localJoints     = skin.unpackedJoints;
            weights         = skin.unpackedWeights;
        }

        uint vertexCount      = this.vertexCount;
        uint morphOffset      = this.morphOffset;
        uint activeMorphCount = this.activeMorphCount;
        float4 morphWeights   = this.morphWeights;
        if (activeMorphCount > 0) {
            localPos.xyz += GetMorphDisplacement(actualVertexId, vertexCount, morphOffset, activeMorphCount, morphWeights);
        }

        float3x3 world3x3        = (float3x3) this.world;
        float4x4 worldMatrix     = this.world;
        float4x4 prevWorldMatrix = this.prevWorld;
        uint jointOffset         = this.jointOffset;

        if (this.isSkinned) {
            worldPos           = mul(worldMatrix, SkinPosition(localPos, localJoints, weights, jointOffset));
            output.normal      = normalize(mul(world3x3, SkinDirection(localNormal, localJoints, weights, jointOffset)));
            output.tangent.xyz = normalize(mul(world3x3, SkinDirection(localTangent.xyz, localJoints, weights, jointOffset)));
        } else {
            worldPos           = mul(worldMatrix, localPos);
            output.normal      = normalize(mul(world3x3, localNormal));
            output.tangent.xyz = normalize(mul(world3x3, localTangent.xyz));
        }

        output.tangent.w            = localTangent.w;
        output.uv                   = localUV;
        output.uv1                  = localUV1;
        output.color                = localColor;
        output.materialIndices      = uint4(this.albedoIndex, this.normalIndex, this.pbrIndex, this.emissiveIndex);
        output.baseColorFactor      = this.baseColorFactor;
        output.emissiveFactor       = this.emissiveFactor;
        output.pbrFactors           = float3(this.metallicFactor, this.roughnessFactor, this.alphaCutoff);
        output.alphaMode            = this.alphaMode;
        output.transmissionParams   = this.transmissionParams();
        output.volumeParams         = this.volumeParams();
        output.volumeParams.z       = AverageWorldScale(this.world);
        output.transmissionTextures = this.transmissionTextures();
        output.coatParams           = float4(this.clearcoatFactor(), this.clearcoatRoughnessFactor(), this.clearcoatNormalScale(), 0.0f);
        output.coatTextures         = uint4(this.clearcoatTexIndex(), this.clearcoatRoughnessTexIndex(), this.clearcoatNormalTexIndex(), 0u);
        output.anisotropyParams     = float2(this.anisotropyStrength, this.anisotropyRotation);
        output.anisotropyTexIndex   = this.anisotropyTexIndex;
        output.samplerCodes         = uint2(this.samplerCodes0, this.samplerCodes1);

        if (withMotion) {
            float4 prevWorldPos;
            if (this.isSkinned) {
                prevWorldPos = mul(prevWorldMatrix, SkinPositionPrev(localPos, localJoints, weights, jointOffset));
            } else {
                prevWorldPos = mul(prevWorldMatrix, localPos);
            }
            float4x4 activeVP = this.isViewmodel ? scene.frame.viewmodelViewProj : scene.frame.viewProj;
            output.currClip = mul(activeVP, worldPos);
            output.prevClip = mul(scene.frame.prevUnjitteredViewProj, prevWorldPos);
        }
        return output;
    }
}

public struct ShadowGeometryOutput {
    public float4 pos : SV_Position;
    public float2 uv : TEXCOORD2;
    public float2 uv1 : TEXCOORD17;
    public nointerpolation uint instanceId : TEXCOORD18;
    public float4 color : COLOR;
    public nointerpolation uint4 materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;
    public nointerpolation uint alphaMode : TEXCOORD7;
    public nointerpolation uint2 samplerCodes : TEXCOORD16;
}

public struct GBufferGeometryOutput {
    public float4 pos : SV_Position;
    public float4 currClip : TEXCOORD0;
    public float4 prevClip : TEXCOORD1;
    public float3 normal : NORMAL;
    public float4 tangent : TANGENT;
    public float2 uv : TEXCOORD2;
    public float2 uv1 : TEXCOORD17;
    public nointerpolation uint instanceId : TEXCOORD18;
    public float4 color : COLOR;
    public nointerpolation uint4 materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;
    public nointerpolation uint alphaMode : TEXCOORD7;
    public nointerpolation float4 emissiveFactor : TEXCOORD8;
    public nointerpolation float4 coatParams : TEXCOORD12;
    public nointerpolation uint4 coatTextures : TEXCOORD13;
    public nointerpolation float2 anisotropyParams : TEXCOORD14;
    public nointerpolation uint anisotropyTexIndex : TEXCOORD15;
    public nointerpolation uint2 samplerCodes : TEXCOORD16;
}

public struct ForwardGeometryOutput {
    public float4 pos : SV_Position;
    public float3 normal : NORMAL;
    public float2 uv : TEXCOORD2;
    public float2 uv1 : TEXCOORD17;
    public nointerpolation uint instanceId : TEXCOORD18;
    public float4 color : COLOR;
    public nointerpolation uint4 materialIndices : TEXCOORD4;
    public nointerpolation float4 baseColorFactor : TEXCOORD5;
    public nointerpolation float3 pbrFactors : TEXCOORD6;
    public nointerpolation float4 emissiveFactor : TEXCOORD8;
    public nointerpolation float4 transmissionParams : TEXCOORD9;
    public nointerpolation float4 volumeParams : TEXCOORD10;
    public nointerpolation uint4 transmissionTextures : TEXCOORD11;
    public float4 tangent : TANGENT;
    public nointerpolation float2 anisotropyParams : TEXCOORD14;
    public nointerpolation uint anisotropyTexIndex : TEXCOORD15;
    public nointerpolation uint2 samplerCodes : TEXCOORD16;
}

public ShadowGeometryOutput PackShadowOutput(SceneGeometryVertex v) {
    ShadowGeometryOutput o;
    o.pos              = v.pos;
    o.uv               = v.uv;
    o.uv1 = v.uv1;
    o.instanceId = v.instanceId;
    o.color            = v.color;
    o.materialIndices  = v.materialIndices;
    o.baseColorFactor  = v.baseColorFactor;
    o.pbrFactors       = v.pbrFactors;
    o.alphaMode        = v.alphaMode;
    o.samplerCodes     = v.samplerCodes;
    return o;
}

public GBufferGeometryOutput PackGBufferOutput(SceneGeometryVertex v) {
    GBufferGeometryOutput o;
    o.pos              = v.pos;
    o.currClip         = v.currClip;
    o.prevClip         = v.prevClip;
    o.normal           = v.normal;
    o.tangent          = v.tangent;
    o.uv               = v.uv;
    o.uv1 = v.uv1;
    o.instanceId = v.instanceId;
    o.color            = v.color;
    o.materialIndices  = v.materialIndices;
    o.baseColorFactor  = v.baseColorFactor;
    o.pbrFactors       = v.pbrFactors;
    o.alphaMode        = v.alphaMode;
    o.emissiveFactor      = v.emissiveFactor;
    o.coatParams          = v.coatParams;
    o.coatTextures        = v.coatTextures;
    o.anisotropyParams    = v.anisotropyParams;
    o.anisotropyTexIndex  = v.anisotropyTexIndex;
    o.samplerCodes        = v.samplerCodes;
    return o;
}

public ForwardGeometryOutput PackForwardOutput(SceneGeometryVertex v) {
    ForwardGeometryOutput o;
    o.pos                 = v.pos;
    o.normal              = v.normal;
    o.uv                  = v.uv;
    o.uv1 = v.uv1;
    o.instanceId = v.instanceId;
    o.color               = v.color;
    o.materialIndices     = v.materialIndices;
    o.baseColorFactor     = v.baseColorFactor;
    o.pbrFactors          = v.pbrFactors;
    o.emissiveFactor         = v.emissiveFactor;
    o.transmissionParams     = v.transmissionParams;
    o.volumeParams           = v.volumeParams;
    o.transmissionTextures   = v.transmissionTextures;
    o.tangent                = v.tangent;
    o.anisotropyParams       = v.anisotropyParams;
    o.anisotropyTexIndex     = v.anisotropyTexIndex;
    o.samplerCodes           = v.samplerCodes;
    return o;
}

public interface IScenePassOutput {
    associatedtype Output;
    static Output Emit(SceneGeometryVertex v, uint viewId);
}

public struct ShadowPassOutput : IScenePassOutput {
    typealias Output = ShadowGeometryOutput;
    public static Output Emit(SceneGeometryVertex v, uint viewId) {
        return PackShadowOutput(v);
    }
}

public struct GBufferPassOutput : IScenePassOutput {
    typealias Output = GBufferGeometryOutput;
    public static Output Emit(SceneGeometryVertex v, uint viewId) {
        return PackGBufferOutput(v);
    }
}

public struct ForwardPassOutput : IScenePassOutput {
    typealias Output = ForwardGeometryOutput;
    public static Output Emit(SceneGeometryVertex v, uint viewId) {
        return PackForwardOutput(v);
    }
}

public struct MeshletTaskPayload {
    public uint instanceId;
    public uint baseMeshlet;
    public uint meshletIndices[32];
};
