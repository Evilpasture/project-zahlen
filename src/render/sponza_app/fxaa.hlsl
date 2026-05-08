[[vk::binding(0, 0)]] Texture2D sceneTexture;
[[vk::binding(1, 0)]] SamplerState sceneSampler;

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput vout;
    // Standard Vulkan Fullscreen Triangle
    vout.uv = float2((id << 1) & 2, id & 2);
    vout.pos = float4(vout.uv * float2(2.0f, 2.0f) + float2(-1.0f, -1.0f), 0.0f, 1.0f);
    return vout;
}

// ---------------------------------------------------------
// HDR to LDR Tonemapping
// ---------------------------------------------------------
// ACES Filmic Tone Mapping Curve. 
// Smoothly compresses high HDR values down to a nice [0.0, 1.0] range.
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

// Helper function to read the HDR texture and immediately tonemap it.
// You can adjust the "exposure" multiplier here to brighten/darken your scene.
float3 SampleTonemapped(float2 uv) {
    float exposure = 1.0f; // Tweak this if your scene is too dark/bright!
    float3 hdrColor = sceneTexture.Sample(sceneSampler, uv).rgb * exposure;
    return ACESFilm(hdrColor);
}

float GetLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

// ---------------------------------------------------------
// FXAA Pass
// ---------------------------------------------------------
float4 PSMain(VSOutput input) : SV_TARGET {
    float2 res;
    sceneTexture.GetDimensions(res.x, res.y);
    float2 texelSize = 1.0 / res;

    // Fetch tonemapped (LDR) samples instead of raw HDR samples
    float3 rgbNW = SampleTonemapped(input.uv + float2(-1, -1) * texelSize);
    float3 rgbNE = SampleTonemapped(input.uv + float2(1, -1) * texelSize);
    float3 rgbSW = SampleTonemapped(input.uv + float2(-1, 1) * texelSize);
    float3 rgbSE = SampleTonemapped(input.uv + float2(1, 1) * texelSize);
    float3 rgbM  = SampleTonemapped(input.uv);

    float lumaNW = GetLuma(rgbNW);
    float lumaNE = GetLuma(rgbNE);
    float lumaSW = GetLuma(rgbSW);
    float lumaSE = GetLuma(rgbSE);
    float lumaM  = GetLuma(rgbM);

    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));

    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * 0.03125, 0.0078125);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(float2(8, 8), max(float2(-8, -8), dir * rcpDirMin)) * texelSize;

    // Also fetch tonemapped samples for the subpixel blend
    float3 rgbA = 0.5 * (
        SampleTonemapped(input.uv + dir * (1.0/3.0 - 0.5)) +
        SampleTonemapped(input.uv + dir * (2.0/3.0 - 0.5))
    );
    
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        SampleTonemapped(input.uv + dir * -0.5) +
        SampleTonemapped(input.uv + dir * 0.5)
    );

    float lumaB = GetLuma(rgbB);
    if ((lumaB < lumaMin) || (lumaB > lumaMax)) return float4(rgbA, 1.0);
    return float4(rgbB, 1.0);
}