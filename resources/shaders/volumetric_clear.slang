// resources/shaders/volumetric_clear.slang

import volumetric_grid;

[[vk::constant_id(1000)]] const uint ZHLN_DISPATCH_SIZE_X = VOLUMETRIC_GRID_WIDTH;
[[vk::constant_id(1001)]] const uint ZHLN_DISPATCH_SIZE_Y = VOLUMETRIC_GRID_HEIGHT;
[[vk::constant_id(1002)]] const uint ZHLN_DISPATCH_SIZE_Z = VOLUMETRIC_GRID_DEPTH;

RWTexture3D<float4> outVoxelMedia;
RWTexture3D<float4> outVoxelLight;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim) || any(tid >= uint3(ZHLN_DISPATCH_SIZE_X, ZHLN_DISPATCH_SIZE_Y, ZHLN_DISPATCH_SIZE_Z)))
        return;

    outVoxelMedia[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
