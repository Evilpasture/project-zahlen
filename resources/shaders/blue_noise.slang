// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module blue_noise;

public static const float2 R2_OFFSET = float2(0.7548776662466927f, 0.5698402909980532f);

public static const float2 R2_SAMPLE_OFFSET = float2(0.1548776662466927f, 0.3569840290998053f);

public static const float FRAME_TIME_STEP = 0.015625f;
public static const float FRAME_RATE_HZ   = 64.0f;

public static const uint FRAME_INDEX_PERIOD = 4096u;

[ForceInline]
public uint FrameIndexFromCamPosW(float camPosW) {
    return uint(max(camPosW, 0.0f) * FRAME_RATE_HZ + 0.5f) & (FRAME_INDEX_PERIOD - 1u);
}

[ForceInline]
public float4 SampleBlueNoise(Texture2D<float4> noiseTex, SamplerState repeatPoint, uint2 pixelPos, uint frameIndex) {
    uint width = 0u, height = 0u;
    noiseTex.GetDimensions(width, height);
    float2 invExtent = 1.0f / float2(float(max(width, 1u)), float(max(height, 1u)));

    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET);
    float2 uv = (float2(pixelPos) + 0.5f) * invExtent + scroll;
    return noiseTex.SampleLevel(repeatPoint, uv, 0.0f);
}

[ForceInline]
public float4 SampleBlueNoiseAt(Texture2D<float4> noiseTex, SamplerState repeatPoint, uint2 pixelPos, uint frameIndex, uint sampleIndex) {
    uint width = 0u, height = 0u;
    noiseTex.GetDimensions(width, height);
    float2 invExtent = 1.0f / float2(float(max(width, 1u)), float(max(height, 1u)));

    float2 scroll = frac(float2(float(frameIndex), float(frameIndex)) * R2_OFFSET +
                         float2(float(sampleIndex), float(sampleIndex)) * R2_SAMPLE_OFFSET);
    float2 uv     = (float2(pixelPos) + 0.5f) * invExtent + scroll;
    return noiseTex.SampleLevel(repeatPoint, uv, 0.0f);
}

[ForceInline]
public float2 VogelDisk(uint i, uint n, float rotation) {
    const float goldenAngle = 2.399963229728653f;
    float r                 = sqrt((float(i) + 0.5f) / float(max(n, 1u)));
    float theta             = float(i) * goldenAngle + rotation;
    return float2(cos(theta), sin(theta)) * r;
}
