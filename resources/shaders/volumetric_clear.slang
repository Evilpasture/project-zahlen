// resources/shaders/volumetric_clear.slang

import volumetric_grid;

namespace Dispatch {
    [[vk::constant_id(1000)]] const uint SizeX = Volumetric.Width;
    [[vk::constant_id(1001)]] const uint SizeY = Volumetric.Height;
    [[vk::constant_id(1002)]] const uint SizeZ = Volumetric.Depth;
}

RWTexture3D<float4> outVoxelMedia;
RWTexture3D<float4> outVoxelLight;

[shader("compute")]
[numthreads(8, 8, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    uint3 gridDim;
    outVoxelMedia.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (any(tid >= gridDim) || any(tid >= uint3(Dispatch.SizeX, Dispatch.SizeY, Dispatch.SizeZ)))
        return;

    outVoxelMedia[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
    outVoxelLight[tid] = float4(0.0f, 0.0f, 0.0f, 0.0f);
}
