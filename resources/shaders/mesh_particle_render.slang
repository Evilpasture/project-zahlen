// resources/shaders/mesh_particle_render.hlsl

import common;
import pbr_helpers;
import uniforms;

struct Particle3D {
    float4 position; // xyz = pos, w = scale
    float4 velocity; // xyz = vel, w = unused
    float4 rotation; // xyzw = quaternion
    float4 rotVel;   // xyz = angular velocity, w = unused
    float4 color;    // rgba
    float4 params;   // x = age, y = life
};

struct MeshParticleRenderPushConstants {
    uint64_t particleBufferAddr;
    uint64_t posAddress;
    uint64_t attrAddress;
    uint64_t iboAddress;

    float4 baseColorFactor; // Offset 32 (16-byte aligned)
    float4 emissiveFactor;  // Offset 48 (16-byte aligned)

    uint  indexCount; // Offset 64
    uint  albedoIdx;
    uint  normalIdx;
    uint  pbrIdx;
    uint  emissiveIdx;
    float roughness;
    float metallic;
    float alphaCutoff;
    uint  alphaMode;

    uint _padding; // Offset 100
};

[vk::push_constant] MeshParticleRenderPushConstants mp_pc;

// Bindless registry now comes from the `scene` parameter block (common.slang).

struct VSOutput {
    float4                 pos : SV_Position;
    float4                 currClip : TEXCOORD0;
    float4                 prevClip : TEXCOORD1;
    float3                 worldPos : POSITION;
    float3                 normal : NORMAL;
    float4                 tangent : TANGENT;
    float2                 uv : TEXCOORD2;
    float4                 shadowPos : TEXCOORD3;
    float4                 color : COLOR;
    nointerpolation uint4  materialIndices : TEXCOORD4;
    nointerpolation float4 baseColorFactor : TEXCOORD5;
    nointerpolation float3 pbrFactors : TEXCOORD6;
    nointerpolation uint   alphaMode : TEXCOORD7;
    nointerpolation float4 emissiveFactor : TEXCOORD8;
};

struct PSOutput {
    float4 color : SV_Target0;
    float2 velocity : SV_Target1;
    float4 normalRoughness : SV_Target2;
};

public struct Quaternion {
    public float4 value;
    public init(float4 value) { this.value = value; }

    [ForceInline] public float3 rotate(float3 vector) {
        float3 t = 2.0f * cross(this.value.xyz, vector);
        return vector + this.value.w * t + cross(this.value.xyz, t);
    }
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uint viewId : SV_ViewID) {
    VSOutput output;

    Particle3D* particles = (Particle3D*)mp_pc.particleBufferAddr;
    Particle3D p = particles[instanceId];

    // Cull dead particles cleanly (0-area degenerate clip position)
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
        return output;
    }

    uint actualVertexId = vertexId;
    if (mp_pc.iboAddress != 0) {
        uint* indices = (uint*)mp_pc.iboAddress;
        actualVertexId = indices[vertexId];
    }

    float3* positions = (float3*)mp_pc.posAddress;
    float3 localPos = positions[actualVertexId];
    float3 scaledPos  = localPos * p.position.w;
    float3 rotatedPos = Quaternion(p.rotation).rotate(scaledPos);
    float3 worldPos   = rotatedPos + p.position.xyz;

    output.worldPos = worldPos;
    output.pos      = mul(scene.frame.viewProj, float4(worldPos, 1.0f));
    output.currClip = output.pos;
    output.prevClip = mul(scene.frame.prevUnjitteredViewProj, float4(worldPos, 1.0f));

    uint cascadeIndex = clamp(viewId, 0u, 3u);
    output.shadowPos  = mul(scene.frame.lightSpaceMatrices[cascadeIndex], float4(worldPos, 1.0f));

    if (mp_pc.attrAddress != 0) {
        VertexAttributes* attributes = (VertexAttributes*)mp_pc.attrAddress;
        VertexAttributes attributesAtVertex = attributes[actualVertexId];
        Quaternion rotation = Quaternion(p.rotation);

        float3 localNormal = attributesAtVertex.unpackedNormal.xyz;
        output.normal      = normalize(rotation.rotate(localNormal));

        float4 localTangent = attributesAtVertex.unpackedTangent;
        output.tangent.xyz  = normalize(rotation.rotate(localTangent.xyz));
        output.tangent.w    = localTangent.w;
        output.uv           = attributesAtVertex.unpackedUV;
    } else {
        output.normal  = float3(0.0f, 1.0f, 0.0f);
        output.tangent = float4(1.0f, 0.0f, 0.0f, 1.0f);
        output.uv      = float2(0.0f, 0.0f);
    }

    output.color           = p.color;
    output.materialIndices = uint4(mp_pc.albedoIdx, mp_pc.normalIdx, mp_pc.pbrIdx, mp_pc.emissiveIdx);
    output.baseColorFactor = mp_pc.baseColorFactor;
    output.emissiveFactor  = mp_pc.emissiveFactor;
    output.pbrFactors      = float3(mp_pc.metallic, mp_pc.roughness, mp_pc.alphaCutoff);
    output.alphaMode       = mp_pc.alphaMode;

    return output;
}

void PSShadow(VSOutput input) {
    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float  alphaCutoff     = input.pbrFactors.z;
    uint   alphaMode       = input.alphaMode;

    float4 albedo = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }
}

PSOutput PSMain(VSOutput input) {
    PSOutput output;

    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float  metallicFactor  = input.pbrFactors.x;
    float  roughnessFactor = input.pbrFactors.y;
    float  alphaCutoff     = input.pbrFactors.z;
    uint   alphaMode       = input.alphaMode;

    float4 albedo      = scene.globalTextures[indices.x].Sample(scene.defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = scene.globalTextures[indices.w].Sample(scene.defaultSampler, input.uv).rgb;
    float3 emissive    = emissiveMap * input.emissiveFactor.rgb;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }

    float4 pbr       = scene.globalTextures[indices.z].Sample(scene.defaultSampler, input.uv);
    float  roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
    float  metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    float3 N           = normalize(input.normal);
    float3 worldNormal = N;

    if (indices.y != 2 && any(input.tangent.xyz)) {
        float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
        if (dot(T_unnorm, T_unnorm) < 0.0001f) {
            T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
        }
        float3 T           = normalize(T_unnorm);
        float  tangentSign = input.tangent.w * 2.0f - 1.0f;
        float3 B           = normalize(cross(N, T) * tangentSign);

        float3 normalMap = scene.globalTextures[indices.y].Sample(scene.defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        worldNormal      = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
    }

    output.color           = float4(albedo.rgb + emissive, albedo.a);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    float  currW   = max(input.currClip.w, 0.0001f);
    float  prevW   = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}
