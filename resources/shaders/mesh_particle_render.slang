
import common;
import gpu_buffer;
import material_model;
import math.transform;
import pbr_helpers;
import uniforms;

struct Particle3D {
    float4 position;
    float4 velocity;
    float4 rotation;
    float4 rotVel;
    float4 color;
    float4 params;
};

struct MeshParticleRenderPushConstants {
    uint64_t particleBufferAddr;
    uint64_t posAddress;
    float4 baseColorFactor;
    float4 emissiveFactor;
    uint64_t tangentFrameAddress;
    uint64_t surfaceAddress;
    uint64_t iboAddress;

    uint indexCount;
    uint albedoIdx;
    uint normalIdx;
    uint pbrIdx;
    uint emissiveIdx;
    float roughness;
    float metallic;
    float alphaCutoff;
    uint alphaMode;

    uint samplerCodes0;
    uint samplerCodes1;
    uint unlit;
};

[vk::push_constant]
MeshParticleRenderPushConstants mp_pc;


struct VSOutput {
    float4 pos : SV_Position;
    float4 currClip : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
    float3 worldPos : POSITION;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD2;
    float4 shadowPos : TEXCOORD3;
    float4 color : COLOR;
    nointerpolation uint4 materialIndices : TEXCOORD4;
    nointerpolation float4 baseColorFactor : TEXCOORD5;
    nointerpolation float3 pbrFactors : TEXCOORD6;
    nointerpolation uint alphaMode : TEXCOORD7;
    nointerpolation float4 emissiveFactor : TEXCOORD8;
    nointerpolation uint2 samplerCodes : TEXCOORD16;
};

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
    float4 emissive : SV_Target3;
    float4 clearcoat : SV_Target4;
    float4 anisotropy : SV_Target5;
    float4 sheen : SV_Target6;
};

VSOutput VSMain(uint vertexId: SV_VertexID, uint instanceId: SV_InstanceID, uint viewId: SV_ViewID) {
    VSOutput output;

    GPUBuffer<Particle3D> particles = GPUBuffer<Particle3D>(mp_pc.particleBufferAddr);
    Particle3D p = particles.load(instanceId);

    if (p.params.x >= p.params.y || p.params.y <= 0.0f) {
        output.pos             = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.currClip        = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.prevClip        = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.worldPos        = float3(0.0f, 0.0f, 0.0f);
        output.normal          = float3(0.0f, 1.0f, 0.0f);
        output.tangent         = float4(1.0f, 0.0f, 0.0f, 1.0f);
        output.uv              = float2(0.0f, 0.0f);
        output.shadowPos       = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.color           = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.materialIndices = uint4(0, 0, 0, 0);
        output.baseColorFactor = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.pbrFactors      = float3(0.0f, 0.0f, 0.0f);
        output.alphaMode       = 0;
        output.emissiveFactor  = float4(0.0f, 0.0f, 0.0f, 0.0f);
        output.samplerCodes    = uint2(0u, 0u);
        return output;
    }

    uint actualVertexId = vertexId;
    if (mp_pc.iboAddress != 0) {
        GPUBuffer<uint> indices = GPUBuffer<uint>(mp_pc.iboAddress);
        actualVertexId = indices.load(vertexId);
    }

    GPUBuffer<float> positionValues = GPUBuffer<float>(mp_pc.posAddress);
    uint positionOffset = actualVertexId * 3u;
    float3 localPos = float3(positionValues.load(positionOffset), positionValues.load(positionOffset + 1u), positionValues.load(positionOffset + 2u));
    float3 scaledPos = localPos * p.position.w;
    float3 rotatedPos = Quaternion(p.rotation).rotate(scaledPos);
    float3 worldPos = rotatedPos + p.position.xyz;

    output.worldPos = worldPos;
    output.pos      = mul(scene.frame.viewProj, float4(worldPos, 1.0f));
    output.currClip = output.pos;
    output.prevClip = mul(scene.frame.prevUnjitteredViewProj, float4(worldPos, 1.0f));

    uint cascadeIndex = clamp(viewId, 0u, 3u);
    output.shadowPos  = mul(scene.frame.lightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0f));

#ifdef SHADOW_PASS
    output.pos = output.shadowPos;
#endif

    output.normal  = float3(0.0f, 1.0f, 0.0f);
    output.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
    output.uv      = float2(0.0f, 0.0f);
    if (mp_pc.tangentFrameAddress != 0) {
        VertexTangentFrame frame = GPUBuffer<VertexTangentFrame>(mp_pc.tangentFrameAddress).load(actualVertexId);
        Quaternion rotation = Quaternion(p.rotation);
        output.normal = normalize(rotation.rotate(frame.unpackedNormal.xyz));
        float4 tangent = frame.unpackedTangent;
        output.tangent = float4(normalize(rotation.rotate(tangent.xyz)), tangent.w);
    }
    if (mp_pc.surfaceAddress != 0) {
        VertexSurface surface = GPUBuffer<VertexSurface>(mp_pc.surfaceAddress).load(actualVertexId);
        output.uv = surface.unpackedUV;
    }

    output.color           = p.color;
    output.materialIndices = uint4(mp_pc.albedoIdx, mp_pc.normalIdx, mp_pc.pbrIdx, mp_pc.emissiveIdx);
    output.baseColorFactor = mp_pc.baseColorFactor;
    output.emissiveFactor  = mp_pc.emissiveFactor;
    output.pbrFactors      = float3(mp_pc.metallic, mp_pc.roughness, mp_pc.alphaCutoff);
    output.alphaMode       = mp_pc.alphaMode;
    output.samplerCodes    = uint2(mp_pc.samplerCodes0, mp_pc.samplerCodes1);

    return output;
}

void PSShadow(VSOutput input) {
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, input.emissiveFactor, input.pbrFactors, input.alphaMode);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }
}

PSOutput PSMain(VSOutput input) {
    PSOutput output;
    MaterialSampleInputs material = MakeMaterialInputs(input.uv, input.color, input.materialIndices, input.samplerCodes,
                                                        input.baseColorFactor, input.emissiveFactor, input.pbrFactors, input.alphaMode);
    float4 albedo = SampleMaterialAlbedo<StandardPBRMaterial>(material);
    if (RejectMaskedAlpha(material, albedo.a)) {
        discard;
    }
    if (mp_pc.unlit != 0u) {
        output.color           = albedo;
        output.emissive        = float4(0.0f);
        output.normalRoughness = float4(PackNormalOctahedron(normalize(input.normal)) * 0.5f + 0.5f, 1.0f, 0.0f);
        output.clearcoat       = float4(0.0f);
        output.anisotropy      = float4(0.0f, 0.0f, 1.0f, 1.0f);
        output.sheen           = float4(0.0f);
        float currW = max(input.currClip.w, 0.0001f);
        float prevW = max(input.prevClip.w, 0.0001f);
        output.velocity = (input.currClip.xy / currW - input.prevClip.xy / prevW) * 0.5f;
        return output;
    }
    float3 emissive = SampleMaterialEmissive<StandardPBRMaterial>(material);

    // Mesh-particle normals use their original tangent fallback and encoded
    // tangent sign; their G-buffer roughness floor is 0.045 for every mode.
    material.roughnessFloor = 0.045f;
    material.geometricNormal = normalize(input.normal);
    material.hasTangent = any(input.tangent.xyz);
    if (material.normalIndex != 2 && material.hasTangent) {
        float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, material.geometricNormal) * material.geometricNormal;
        if (dot(T_unnorm, T_unnorm) < 0.0001f) {
            T_unnorm = cross(material.geometricNormal, abs(material.geometricNormal.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
        }
        material.tangentDirection = normalize(T_unnorm);
        float tangentSign = input.tangent.w * 2.0f - 1.0f;
        material.bitangentDirection = normalize(cross(material.geometricNormal, material.tangentDirection) * tangentSign);
    }
    SurfaceInteraction surface = EvaluateMaterial<StandardPBRMaterial>(material, albedo, emissive);
    output.color           = float4(surface.albedo, surface.alpha);
    output.emissive        = float4(surface.emissive, 1.0f);
    float2 packedNormal    = PackNormalOctahedron(surface.normal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, surface.roughness, surface.metallic);

    float currW    = max(input.currClip.w, 0.0001f);
    float prevW    = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity  = (ndcCurr - ndcPrev) * 0.5f;
    output.clearcoat  = surface.clearcoat;
    output.anisotropy = float4(surface.anisotropy.xy, EncodeGBufferAnisotropy(surface.anisotropy.z), surface.anisotropy.w);
    output.sheen = surface.sheen;

    return output;
}
