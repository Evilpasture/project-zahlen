// resources/shaders/skinning.slang

struct VertexAttributes {
    uint normal;
    uint tangent;
    uint uv;
    uint color;
};

struct VertexSkin {
    uint2 joints; // Packed 4x uint16_t (8 bytes total)
    uint weights; // Packed UNORM8 weights
};

struct GPUJoint {
    float4 col0;
    float4 col1;
    float4 col2;
    float4 col3;
};

struct SkinningConstants {
    uint64_t inPosAddr;
    uint64_t inAttrAddr;
    uint64_t inSkinAddr;
    uint64_t outPosAddr;
    uint64_t outAttrAddr;
    uint64_t jointsAddr;
    uint64_t morphDeltasAddr;
    uint32_t vertexCount;
    uint32_t jointOffset;
    uint32_t morphOffset;
    uint32_t activeMorphCount;
    float morphWeights[4];
};

[vk::push_constant]
SkinningConstants pcs;

float4 UnpackNormal(uint packed) {
    float x = (float(packed & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float y = (float((packed >> 10) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float z = (float((packed >> 20) & 0x3FF) / 1023.0f) * 2.0f - 1.0f;
    float w = (packed >> 30) > 0 ? 1.0f : -1.0f;
    return float4(x, y, z, w);
}

uint PackNormal(float3 n, float w) {
    uint xs = (uint) ((n.x * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint ys = (uint) ((n.y * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint zs = (uint) ((n.z * 0.5f + 0.5f) * 1023.0f) & 0x3FF;
    uint ws = (uint) (w > 0 ? 3 : 0) & 0x3;
    return (ws << 30) | (zs << 20) | (ys << 10) | xs;
}

float4 SkinPosition(float4 position, uint4 joints, float4 weights, uint jointOffset, GPUJoint* jointsPool) {
    GPUJoint j0 = jointsPool[jointOffset + joints.x];
    GPUJoint j1 = jointsPool[jointOffset + joints.y];
    GPUJoint j2 = jointsPool[jointOffset + joints.z];
    GPUJoint j3 = jointsPool[jointOffset + joints.w];

    float4 pos = (j0.col0 * position.x + j0.col1 * position.y + j0.col2 * position.z + j0.col3 * position.w) * weights.x +
                 (j1.col0 * position.x + j1.col1 * position.y + j1.col2 * position.z + j1.col3 * position.w) * weights.y +
                 (j2.col0 * position.x + j2.col1 * position.y + j2.col2 * position.z + j2.col3 * position.w) * weights.z +
                 (j3.col0 * position.x + j3.col1 * position.y + j3.col2 * position.z + j3.col3 * position.w) * weights.w;
    return pos;
}

float3 SkinDirection(float3 direction, uint4 joints, float4 weights, uint jointOffset, GPUJoint* jointsPool) {
    GPUJoint j0 = jointsPool[jointOffset + joints.x];
    GPUJoint j1 = jointsPool[jointOffset + joints.y];
    GPUJoint j2 = jointsPool[jointOffset + joints.z];
    GPUJoint j3 = jointsPool[jointOffset + joints.w];

    float3 dir = (j0.col0.xyz * direction.x + j0.col1.xyz * direction.y + j0.col2.xyz * direction.z) * weights.x +
                 (j1.col0.xyz * direction.x + j1.col1.xyz * direction.y + j1.col2.xyz * direction.z) * weights.y +
                 (j2.col0.xyz * direction.x + j2.col1.xyz * direction.y + j2.col2.xyz * direction.z) * weights.z +
                 (j3.col0.xyz * direction.x + j3.col1.xyz * direction.y + j3.col2.xyz * direction.z) * weights.w;
    return dir;
}

uint4 UnpackJoints(uint2 packed) {
    return uint4(packed.x & 0xFFFF, packed.x >> 16, packed.y & 0xFFFF, packed.y >> 16);
}

float4 UnpackWeights(uint packed) {
    return float4(float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f,
                  float((packed >> 24) & 0xFF) / 255.0f);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pcs.vertexCount)
        return;

    // Convert raw 64-bit addresses to strongly-typed GPU pointers
    float3* inPosPool             = (float3*) pcs.inPosAddr;
    VertexAttributes* inAttrPool  = (VertexAttributes*) pcs.inAttrAddr;
    VertexSkin* inSkinPool        = (VertexSkin*) pcs.inSkinAddr;
    float3* outPosPool            = (float3*) pcs.outPosAddr;
    VertexAttributes* outAttrPool = (VertexAttributes*) pcs.outAttrAddr;
    GPUJoint* jointsPool          = (GPUJoint*) pcs.jointsAddr;
    float4* morphDeltasPool       = (float4*) pcs.morphDeltasAddr;

    // Load separated SoA input streams using raw array subscripting
    float3 localPos       = inPosPool[tid.x];
    VertexAttributes attr = inAttrPool[tid.x];

    uint normalRaw  = attr.normal;
    uint tangentRaw = attr.tangent;
    uint uvRaw      = attr.uv;
    uint colorRaw   = attr.color;

    uint4 joints   = uint4(0, 0, 0, 0);
    float4 weights = float4(1.0f, 0.0f, 0.0f, 0.0f);

    if (pcs.inSkinAddr != 0) {
        VertexSkin skin = inSkinPool[tid.x];
        joints          = UnpackJoints(skin.joints);
        weights         = UnpackWeights(skin.weights);
    }

    // Apply Morph Targets FIRST (in bind-pose space)
    if (pcs.activeMorphCount > 0) {
        [unroll]
        for (uint i = 0; i < 4; ++i) {
            if (i >= pcs.activeMorphCount)
                break;
            uint deltaIndex = pcs.morphOffset + (i * pcs.vertexCount) + tid.x;

            float weight = pcs.morphWeights[i];
            float3 delta = morphDeltasPool[deltaIndex].xyz;
            localPos += delta * weight;
        }
    }

    // Apply joint matrices SECOND
    float4 skinnedPos  = SkinPosition(float4(localPos, 1.0f), joints, weights, pcs.jointOffset, jointsPool);
    float4 normUnpack  = UnpackNormal(normalRaw);
    float3 skinnedNorm = normalize(SkinDirection(normUnpack.xyz, joints, weights, pcs.jointOffset, jointsPool));
    float4 tangUnpack  = UnpackNormal(tangentRaw);
    float3 skinnedTang = normalize(SkinDirection(tangUnpack.xyz, joints, weights, pcs.jointOffset, jointsPool));

    // Write results directly to output scratch buffers via pointer offsets
    outPosPool[tid.x] = skinnedPos.xyz;

    uint outNorm       = PackNormal(skinnedNorm, normUnpack.w);
    uint outTang       = PackNormal(skinnedTang, tangUnpack.w);
    outAttrPool[tid.x] = { outNorm, outTang, uvRaw, colorRaw };
}
