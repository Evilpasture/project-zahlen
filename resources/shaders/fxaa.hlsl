#pragma pack_matrix(column_major)

[[vk::binding(0, 0)]] Texture2D sceneColor;
[[vk::binding(1, 0)]] SamplerState defaultSampler;

struct VSInput { uint vID : SV_VertexID; };
struct PSInput { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// --- Vertex Shader (The Fullscreen Triangle Trick) ---
PSInput VSMain(VSInput input) {
    PSInput output;
    // Generates a triangle that covers the whole screen
    output.uv = float2((input.vID << 1) & 2, input.vID & 2);
    output.pos = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

// --- FXAA Helper: Calculate Luminance ---
float Luma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

// --- FXAA Pixel Shader ---
float4 PSMain(PSInput input) : SV_Target {
    float2 res;
    sceneColor.GetDimensions(res.x, res.y);
    float2 rcpFrame = 1.0 / res;

    // 1. Sample neighbors
    float3 rgbM = sceneColor.Sample(defaultSampler, input.uv).rgb;
    float3 rgbNW = sceneColor.Sample(defaultSampler, input.uv + float2(-1, -1) * rcpFrame).rgb;
    float3 rgbNE = sceneColor.Sample(defaultSampler, input.uv + float2(1, -1) * rcpFrame).rgb;
    float3 rgbSW = sceneColor.Sample(defaultSampler, input.uv + float2(-1, 1) * rcpFrame).rgb;
    float3 rgbSE = sceneColor.Sample(defaultSampler, input.uv + float2(1, 1) * rcpFrame).rgb;

    // 2. Convert to luminance
    float lumaM  = Luma(rgbM);
    float lumaNW = Luma(rgbNW);
    float lumaNE = Luma(rgbNE);
    float lumaSW = Luma(rgbSW);
    float lumaSE = Luma(rgbSE);

    // 3. Find min/max luma to detect if we're even on an edge
    float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    float range = lumaMax - lumaMin;

    // If contrast is low, skip AA and just return original pixel
    if (range < max(0.063, lumaMax * 0.125)) {
        return float4(rgbM, 1.0);
    }

    // 4. Calculate blurred color (approximate sampling)
    float2 dir;
    dir.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    dir.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));

    float dirReduce = max((lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 * 0.125), 0.0078125);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);

    dir = min(float2(8.0, 8.0), max(float2(-8.0, -8.0), dir * rcpDirMin)) * rcpFrame;

    // 5. Final samples along the edge direction
    float3 rgbA = 0.5 * (
        sceneColor.Sample(defaultSampler, input.uv + dir * (1.0/3.0 - 0.5)).rgb +
        sceneColor.Sample(defaultSampler, input.uv + dir * (2.0/3.0 - 0.5)).rgb);
        
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        sceneColor.Sample(defaultSampler, input.uv + dir * (0.0/3.0 - 0.5)).rgb +
        sceneColor.Sample(defaultSampler, input.uv + dir * (3.0/3.0 - 0.5)).rgb);

    float lumaB = Luma(rgbB);
    if ((lumaB < lumaMin) || (lumaB > lumaMax)) {
        return float4(rgbA, 1.0);
    } else {
        return float4(rgbB, 1.0);
    }
}