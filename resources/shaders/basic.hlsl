// resources/shaders/basic.hlsl
#include "common.hlsl"

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID, uint viewId : SV_ViewID) {
    VSOutput output;

    uint         instId = (obj.instanceId != 4294967295u) ? obj.instanceId : instanceId;
    InstanceData inst   = g_instances[instId];

    uint actualVertexId = vertexId;
    if (inst.iboAddress != 0) {
        actualVertexId = vk::RawBufferLoad<uint>(inst.iboAddress + vertexId * 4, 4);
    }

    uint texIndices0 = inst.texIndices0;
    uint albedoIdx   = texIndices0 & 0xFFFF;
    uint normalIdx   = texIndices0 >> 16;
    uint texIndices1 = inst.texIndices1;
    uint pbrIdx      = texIndices1 & 0xFFFF;
    uint emissiveIdx = texIndices1 >> 16;

    uint flags     = inst.flags;
    uint alphaMode = flags & 0xFF;
    uint isSkinned = (flags >> 8) & 0xFF;

    float3 position     = vk::RawBufferLoad<float3>(inst.posAddress + actualVertexId * 12, 4);
    float2 localUV      = float2(0, 0);
    float4 localColor   = float4(1, 1, 1, 1);
    float3 localNormal  = float3(0, 1, 0);
    float4 localTangent = float4(1, 0, 0, 1);

    // Load Attributes
    if (inst.attrAddress != 0) {
        uint64_t attrAddr   = inst.attrAddress + actualVertexId * 16;
        uint     normalRaw  = vk::RawBufferLoad<uint>(attrAddr + 0, 4);
        uint     tangentRaw = vk::RawBufferLoad<uint>(attrAddr + 4, 4);
        uint     uvRaw      = vk::RawBufferLoad<uint>(attrAddr + 8, 4);
        uint     colorRaw   = vk::RawBufferLoad<uint>(attrAddr + 12, 4);

        localNormal  = UnpackNormal(normalRaw).xyz;
        localTangent = UnpackNormal(tangentRaw);
        localUV      = UnpackUV(uvRaw);
        localColor   = UnpackColor(colorRaw);
    }

    float4 localPos = float4(position, 1.0f);

    uint4  localJoints = uint4(0, 0, 0, 0);
    float4 weights     = float4(0.0f, 0.0f, 0.0f, 0.0f);

    // Unpack UNORM8 skinning weights
    if (isSkinned != 0 && inst.skinAddress != 0) {
        uint64_t skinAddr   = inst.skinAddress + actualVertexId * 12;
        uint2    jointsRaw  = vk::RawBufferLoad<uint2>(skinAddr + 0, 4);
        uint     weightsRaw = vk::RawBufferLoad<uint>(skinAddr + 8, 4);

        localJoints = UnpackJoints(jointsRaw);
        weights     = UnpackColor(weightsRaw); // Decodes UNORM8 weights!
    }

    uint   vertexCount      = inst.vertexCount;
    uint   morphOffset      = inst.morphOffset;
    uint   activeMorphCount = inst.activeMorphCount;
    float4 morphWeights     = inst.morphWeights;

    if (activeMorphCount > 0) {
        localPos.xyz += GetMorphDisplacement(actualVertexId, vertexCount, morphOffset, activeMorphCount, morphWeights);
    }

    float4   worldPos;
    float3x3 world3x3        = (float3x3) inst.world;
    float4x4 worldMatrix     = inst.world;
    float4x4 prevWorldMatrix = inst.prevWorld;
    uint     jointOffset     = inst.jointOffset;

    if (isSkinned != 0) {
        worldPos           = mul(worldMatrix, SkinPosition(localPos, localJoints, weights, jointOffset));
        output.normal      = normalize(mul(world3x3, SkinDirection(localNormal, localJoints, weights, jointOffset)));
        output.tangent.xyz = normalize(mul(world3x3, SkinDirection(localTangent.xyz, localJoints, weights, jointOffset)));
    } else {
        worldPos           = mul(worldMatrix, localPos);
        output.normal      = normalize(mul(world3x3, localNormal));
        output.tangent.xyz = normalize(mul(world3x3, localTangent.xyz));
    }

    output.worldPos = worldPos.xyz;

    float4 baseColorFactor = inst.baseColorFactor;
    float4 emissiveFactor  = inst.emissiveFactor;
    float  metallicFactor  = inst.metallicFactor;
    float  roughnessFactor = inst.roughnessFactor;
    float  alphaCutoff     = inst.alphaCutoff;

    if (obj.isShadowPass != 0) {
        uint cascadeIndex      = clamp(viewId, 0u, 3u);
        output.pos             = mul(frame.lightSpaceMatrices[cascadeIndex], worldPos);
        output.uv              = localUV;
        output.baseColorFactor = baseColorFactor;
        output.emissiveFactor  = emissiveFactor;
        output.pbrFactors      = float3(metallicFactor, roughnessFactor, alphaCutoff);
        output.alphaMode       = alphaMode;
        output.materialIndices = uint4(albedoIdx, 0, 0, 0);

        output.currClip  = 0;
        output.prevClip  = 0;
        output.normal    = 0;
        output.tangent   = 0;
        output.shadowPos = 0;
        output.color     = localColor;
        return output;
    }

    output.pos      = mul(frame.viewProj, worldPos);
    output.currClip = mul(frame.unjitteredViewProj, worldPos);

    float4 prevWorldPos;
    if (isSkinned != 0) {
        prevWorldPos = mul(prevWorldMatrix, SkinPositionPrev(localPos, localJoints, weights, jointOffset));
    } else {
        prevWorldPos = mul(prevWorldMatrix, localPos);
    }
    output.prevClip = mul(frame.prevUnjitteredViewProj, prevWorldPos);

    output.tangent.w = localTangent.w;
    output.uv        = localUV;

    output.shadowPos       = mul(frame.lightSpaceMatrices[0], worldPos);
    output.color           = localColor;
    output.materialIndices = uint4(albedoIdx, normalIdx, pbrIdx, emissiveIdx);
    output.baseColorFactor = baseColorFactor;
    output.emissiveFactor  = emissiveFactor;
    output.pbrFactors      = float3(metallicFactor, roughnessFactor, alphaCutoff);
    output.alphaMode       = alphaMode;
    return output;
}

void PSShadow(VSOutput input) {
    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;
    float  alphaCutoff     = input.pbrFactors.z;
    uint   alphaMode       = input.alphaMode;

    float4 albedo = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;

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

    float4 albedo      = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;
    float3 emissive    = emissiveMap * input.emissiveFactor.rgb;

    if (alphaMode == 1 && albedo.a < alphaCutoff) {
        discard;
    }

    float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);

    float roughness = max((indices.z == 0 ? 1.0f : pbr.g) * roughnessFactor, 0.045f);
    float metallic  = (indices.z == 0 ? 1.0f : pbr.b) * metallicFactor;

    // === FIX: If the pixel belongs to a skinned mesh, set roughness to exactly 0.0 ===
    if (input.alphaMode == 1 || (obj.instanceId != 4294967295u && (g_instances[obj.instanceId].flags >> 8 & 0xFF) != 0)) {
        roughness = 0.0f; // Magic value indicating "Do Not Reflect"
    }
    // ===============================================================================

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

        float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        worldNormal      = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
    }

    output.color           = float4(albedo.rgb + emissive, albedo.a);
    float2 packedNormal    = PackNormalOctahedron(worldNormal) * 0.5f + 0.5f;
    output.normalRoughness = float4(packedNormal, roughness, metallic);

    float  currW   = max(input.currClip.w, 0.0001f);
    float  prevW   = max(input.prevClip.w, 0.0001f);
    float2 ndcCurr = input.currClip.xy / currW;
    float2 ndcPrev = input.prevClip.xy / prevW;

    // Native Vulkan positive coordinate mapping mapping [-1, 1] to [0, 1]
    output.velocity = (ndcCurr - ndcPrev) * 0.5f;

    return output;
}

#ifdef FORWARD_PASS
float3 RotateVector(float3 V, float3 lightDir) {
    float lenB = length(lightDir);
    if (lenB < 0.001f) {
        return V;
    }
    float3 A     = normalize(float3(0.5f, 1.0f, 0.2f));
    float3 B     = lightDir / lenB;
    float3 q_xyz = cross(B, A);
    float  q_w   = 1.0f + dot(A, B);
    float  q_len = sqrt(dot(q_xyz, q_xyz) + q_w * q_w);
    if (q_len > 0.0001f) {
        q_xyz /= q_len;
        q_w /= q_len;
        return V + 2.0f * cross(q_xyz, cross(q_xyz, V) + q_w * V);
    }
    return V;
}

float4 PSForward(VSOutput input): SV_Target0 {
    uint4  indices         = input.materialIndices;
    float4 baseColorFactor = input.baseColorFactor;

    float4 albedo      = globalTextures[indices.x].Sample(defaultSampler, input.uv) * baseColorFactor * input.color;
    float3 emissiveMap = globalTextures[indices.w].Sample(defaultSampler, input.uv).rgb;
    float3 emissive    = emissiveMap * input.emissiveFactor.rgb;

    // Bypass shading entirely in Fullbright Mode
    if (frame.fullBright != 0) {
        return float4(albedo.rgb + emissive, albedo.a);
    }

    float metallicFactor  = input.pbrFactors.x;
    float roughnessFactor = input.pbrFactors.y;

    float3 N = normalize(input.normal);
    float3 V = normalize(frame.camPos.xyz - input.worldPos);
    float3 R = reflect(-V, N);

    if (indices.z != 0) {
        float4 pbr = globalTextures[indices.z].Sample(defaultSampler, input.uv);
        roughnessFactor *= max(pbr.g, 0.045f);
        metallicFactor *= pbr.b;
    }

    if (indices.y != 2 && any(input.tangent.xyz)) {
        float3 T_unnorm = input.tangent.xyz - dot(input.tangent.xyz, N) * N;
        if (dot(T_unnorm, T_unnorm) < 0.0001f) {
            T_unnorm = cross(N, abs(N.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0));
        }
        float3 T           = normalize(T_unnorm);
        float  tangentSign = input.tangent.w * 2.0f - 1.0f;
        float3 B           = normalize(cross(N, T) * tangentSign);

        float3 normalMap = globalTextures[indices.y].Sample(defaultSampler, input.uv).rgb * 2.0f - 1.0f;
        N                = normalize(normalMap.x * T + normalMap.y * B + normalMap.z * N);
        R                = reflect(-V, N);
    }

    float  NdotV = saturate(dot(N, V));
    float3 F0    = lerp(float3(0.04f, 0.04f, 0.04f), albedo.rgb, metallicFactor);
    float3 F     = F0 + (max(float3(1.0f - roughnessFactor, 1.0f - roughnessFactor, 1.0f - roughnessFactor), F0) - F0) * pow(saturate(1.0f - NdotV), 5.0f);

    float2 envBRDF = brdfLUT.SampleLevel(clampSampler, float2(NdotV, roughnessFactor), 0.0f).rg;

    // Rotate the forward reflection vector to match the dynamic sun
    float3 R_rot = RotateVector(R, frame.lightDir.xyz);

    // Fast Specular IBL Reflection
    float3 specularIBL = prefilteredMap.SampleLevel(defaultSampler, R_rot, roughnessFactor * 5.0f).rgb * 25.0f * (F * envBRDF.x + envBRDF.y);

    float3 diffuseIBL = albedo.rgb * 0.5f;
    float3 finalColor = diffuseIBL + specularIBL + emissive;

    // Additive fresnel boost to simulate refractive edges
    float glassAlpha = saturate(albedo.a + saturate(1.0f - NdotV) * 0.5f);

    return float4(finalColor, glassAlpha);
}
#endif
