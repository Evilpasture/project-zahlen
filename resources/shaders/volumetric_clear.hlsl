// resources/shaders/volumetric_clear.hlsl
#pragma pack_matrix(column_major)

[[vk::binding(0, 0)]] RWTexture3D<float4> outVoxelMedia;
[[vk::binding(1, 0)]] RWTexture3D<float4> outVoxelLight;

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim)) return;

    outVoxelMedia[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
