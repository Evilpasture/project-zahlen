// resources/shaders/fxaa.slang

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

[shader("vertex")] VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);

    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

struct PushConstants {
    float rcpFrameX;
    float rcpFrameY;
    float subpix;
    float edgeThreshold;
    float edgeThresholdMin;
    float _pad;
};

[vk::push_constant] PushConstants pc;

Texture2D<float4> texInput;
SamplerState      smp;

// Convert RGB to Luma (Y) for edge detection, incorporating a soft perceptual
// Reinhard tonemap curve locally so HDR sun glare doesn't smear the edge tracking.
float GetLuma(float3 rgb) {
    float luma = dot(rgb, float3(0.299f, 0.587f, 0.114f));
    return luma / (1.0f + luma);
}

[shader("fragment")] float4 PSMain(VSOutput input): SV_Target0 {
    float3 rgbM = texInput.SampleLevel(smp, input.uv, 0.0f).rgb;

    float lumaM = GetLuma(rgbM);
    float lumaN = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(0, -1)).rgb);
    float lumaS = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(0, 1)).rgb);
    float lumaE = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(1, 0)).rgb);
    float lumaW = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(-1, 0)).rgb);

    float lumaMin   = min(lumaM, min(min(lumaN, lumaS), min(lumaW, lumaE)));
    float lumaMax   = max(lumaM, max(max(lumaN, lumaS), max(lumaW, lumaE)));
    float lumaRange = lumaMax - lumaMin;

    if (lumaRange < max(pc.edgeThresholdMin, lumaMax * pc.edgeThreshold)) {
        return float4(rgbM, 1.0f); // Bail out
    }

    float lumaNW = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(-1, -1)).rgb);
    float lumaNE = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(1, -1)).rgb);
    float lumaSW = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(-1, 1)).rgb);
    float lumaSE = GetLuma(texInput.SampleLevel(smp, input.uv, 0.0f, int2(1, 1)).rgb);

    float lumaL        = (lumaN + lumaS + lumaE + lumaW) * 2.0f;
    float lumaCorners  = lumaNW + lumaNE + lumaSW + lumaSE;
    float subpixFilter = saturate(abs((lumaL + lumaCorners) / 12.0f - lumaM) / lumaRange);
    float subpixBlend  = smoothstep(0.0f, 1.0f, subpixFilter);
    subpixBlend        = subpixBlend * subpixBlend * pc.subpix;

    float edgeHoriz    = abs(-2.0f * lumaN + lumaNW + lumaNE) + abs(-4.0f * lumaM + lumaW + lumaE) * 2.0f + abs(-2.0f * lumaS + lumaSW + lumaSE);
    float edgeVert     = abs(-2.0f * lumaW + lumaNW + lumaSW) + abs(-4.0f * lumaM + lumaN + lumaS) * 2.0f + abs(-2.0f * lumaE + lumaNE + lumaSE);
    bool  isHorizontal = edgeHoriz >= edgeVert;

    float luma1     = isHorizontal ? lumaN : lumaW;
    float luma2     = isHorizontal ? lumaS : lumaE;
    float gradient1 = luma1 - lumaM;
    float gradient2 = luma2 - lumaM;

    bool  is1Steepest    = abs(gradient1) >= abs(gradient2);
    float gradientScaled = 0.25f * max(abs(gradient1), abs(gradient2));

    float stepLength = isHorizontal ? pc.rcpFrameY : pc.rcpFrameX;
    if (is1Steepest)
        stepLength = -stepLength;

    float lumaLocalAverage = 0.0f;
    if (is1Steepest) {
        lumaLocalAverage = 0.5f * (luma1 + lumaM);
    } else {
        lumaLocalAverage = 0.5f * (luma2 + lumaM);
    }

    float2 currentUV = input.uv;
    if (isHorizontal) {
        currentUV.y += stepLength * 0.5f;
    } else {
        currentUV.x += stepLength * 0.5f;
    }

    float2 offset = isHorizontal ? float2(pc.rcpFrameX, 0.0f) : float2(0.0f, pc.rcpFrameY);
    float2 uv1    = currentUV - offset;
    float2 uv2    = currentUV + offset;

    float lumaEnd1 = 0.0f;
    float lumaEnd2 = 0.0f;
    bool  reached1 = false;
    bool  reached2 = false;

    // Quality preset 12 - steps up to 8 texels
    [unroll] for (int i = 0; i < 8; ++i) {
        if (!reached1)
            lumaEnd1 = GetLuma(texInput.SampleLevel(smp, uv1, 0.0f).rgb) - lumaLocalAverage;
        if (!reached2)
            lumaEnd2 = GetLuma(texInput.SampleLevel(smp, uv2, 0.0f).rgb) - lumaLocalAverage;

        reached1 = abs(lumaEnd1) >= gradientScaled;
        reached2 = abs(lumaEnd2) >= gradientScaled;

        if (reached1 && reached2)
            break;

        if (!reached1)
            uv1 -= offset * (i + 1.0f);
        if (!reached2)
            uv2 += offset * (i + 1.0f);
    }

    float dist1 = isHorizontal ? (input.uv.x - uv1.x) : (input.uv.y - uv1.y);
    float dist2 = isHorizontal ? (uv2.x - input.uv.x) : (uv2.y - input.uv.y);

    bool  isDirection1 = dist1 < dist2;
    float distFinal    = min(dist1, dist2);

    float edgeThickness = dist1 + dist2;
    float pixelOffset   = -distFinal / edgeThickness + 0.5f;

    bool  isLumaCenterSmaller = lumaM < lumaLocalAverage;
    bool  isLeftCorrect       = ((isDirection1 ? lumaEnd1 : lumaEnd2) < 0.0f) != isLumaCenterSmaller;
    float finalOffset         = isLeftCorrect ? pixelOffset : 0.0f;

    finalOffset = max(finalOffset, subpixBlend);

    float2 finalUV = input.uv;
    if (isHorizontal) {
        finalUV.y += finalOffset * stepLength;
    } else {
        finalUV.x += finalOffset * stepLength;
    }

    return float4(texInput.SampleLevel(smp, finalUV, 0.0f).rgb, 1.0f);
}
