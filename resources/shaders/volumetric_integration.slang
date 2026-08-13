// resources/shaders/volumetric_integration.slang

Texture3D<float4>   inVoxelLight;
RWTexture3D<float4> outVoxelIntegrated;

[shader("compute")][numthreads(16, 9, 1)] void CSMain(uint3 tid : SV_DispatchThreadID) {
    uint3 gridDim;
    inVoxelLight.GetDimensions(gridDim.x, gridDim.y, gridDim.z);
    if (tid.x >= gridDim.x || tid.y >= gridDim.y)
        return;

    float3 accumulatedScattering    = float3(0.0f, 0.0f, 0.0f);
    float  accumulatedTransmittance = 1.0f;

    float prevDepth = 0.1f; // z_near

    for (uint z = 0; z < gridDim.z; ++z) {
        uint3  sampleCoord  = uint3(tid.xy, z);
        float4 rawLight     = inVoxelLight[sampleCoord];
        float3 inScattering = rawLight.rgb;
        float  extinction   = rawLight.a;

        float nextDepth  = 0.1f * pow(10000.0f, (float(z) + 1.0f) / 64.0f);
        float stepLength = nextDepth - prevDepth;
        prevDepth        = nextDepth;

        float  stepTransmittance = exp(-extinction * stepLength);
        float3 stepIntegrated    = (inScattering - inScattering * stepTransmittance) / max(extinction, 0.00001f);

        accumulatedScattering += accumulatedTransmittance * stepIntegrated;
        accumulatedTransmittance *= stepTransmittance;

        outVoxelIntegrated[sampleCoord] = float4(accumulatedScattering, accumulatedTransmittance);
    }
}
