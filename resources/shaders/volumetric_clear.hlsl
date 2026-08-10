// resources/shaders/volumetric_clear.hlsl
#pragma pack_matrix(column_major) // KEEP: DXC requires this; Slang equivalent is -matrix-layout column_major (or column_major qualifier). See SHADER.md
// Slang note: Slang defaults to row_major memory layout, DXC to column_major. For parity, compile Slang with -matrix-layout column_major or use explicit column_major float4x4.

[[vk::binding(0, 0)]] RWTexture3D<float4> outVoxelMedia;
[[vk::binding(1, 0)]] RWTexture3D<float4> outVoxelLight;

[numthreads(8, 8, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim)) return;

    outVoxelMedia[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
