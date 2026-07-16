// resources/shaders/mlaa.hlsl
#pragma pack_matrix(column_major)

// ============================================================================
// Layout & Push Constants
// ============================================================================

struct MLAAPushConstants {
    float rcpFrameX;
    float rcpFrameY;
    float threshold;
    uint  maxSearchSteps;
};

[[vk::push_constant]] MLAAPushConstants pc;

// Descriptor Layout: DescriptorLayout<SampledImageSlot<0>, SamplerSlot<1>>
[[vk::binding(0, 0)]] Texture2D    colorTex;
[[vk::binding(1, 0)]] SamplerState sPoint;

// ============================================================================
// Structures
// ============================================================================

struct VSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

// ============================================================================
// Vertex Shader Entry Point
// ============================================================================

VSOutput VSMain(uint vertexID : SV_VertexID) {
    VSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    // Flip-free projection; top-left of the texture maps straight to top-left of clip space (-1, -1)
    output.pos = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}

// ============================================================================
// Helper Utilities
// ============================================================================

// Computes perceptual luma using Rec. 709 coefficients
float GetLuma(float3 color) {
    return dot(color, float3(0.2126f, 0.7152f, 0.0722f));
}

// Evaluates whether a local edge exists between two points
bool IsEdge(float lumaA, float lumaB) {
    return abs(lumaA - lumaB) > pc.threshold;
}

// Checks edge presence at a specific offset
bool IsEdgeAtOffset(float2 uv, float2 offset) {
    float3 colA = colorTex.SampleLevel(sPoint, uv, 0).rgb;
    float3 colB = colorTex.SampleLevel(sPoint, uv + offset, 0).rgb;
    return IsEdge(GetLuma(colA), GetLuma(colB));
}

// ============================================================================
// Procedural Edge Search Loops
// ============================================================================

// Search upward along a vertical edge line
float SearchUp(float2 uv, float maxSteps) {
    float2 step      = float2(0.0f, -pc.rcpFrameY);
    float2 currentUV = uv + step;

    [loop] for (uint i = 1; i <= (uint) maxSteps; ++i) {
        float3 colC = colorTex.SampleLevel(sPoint, currentUV, 0).rgb;
        float3 colL = colorTex.SampleLevel(sPoint, currentUV - float2(pc.rcpFrameX, 0.0f), 0).rgb;
        if (!IsEdge(GetLuma(colC), GetLuma(colL))) {
            return (float) i - 0.5f;
        }
        currentUV += step;
    }
    return maxSteps;
}

// Search downward along a vertical edge line
float SearchDown(float2 uv, float maxSteps) {
    float2 step      = float2(0.0f, pc.rcpFrameY);
    float2 currentUV = uv + step;

    [loop] for (uint i = 1; i <= (uint) maxSteps; ++i) {
        float3 colC = colorTex.SampleLevel(sPoint, currentUV, 0).rgb;
        float3 colL = colorTex.SampleLevel(sPoint, currentUV - float2(pc.rcpFrameX, 0.0f), 0).rgb;
        if (!IsEdge(GetLuma(colC), GetLuma(colL))) {
            return (float) i - 0.5f;
        }
        currentUV += step;
    }
    return maxSteps;
}

// Search leftward along a horizontal edge line
float SearchLeft(float2 uv, float maxSteps) {
    float2 step      = float2(-pc.rcpFrameX, 0.0f);
    float2 currentUV = uv + step;

    [loop] for (uint i = 1; i <= (uint) maxSteps; ++i) {
        float3 colC = colorTex.SampleLevel(sPoint, currentUV, 0).rgb;
        float3 colT = colorTex.SampleLevel(sPoint, currentUV - float2(0.0f, pc.rcpFrameY), 0).rgb;
        if (!IsEdge(GetLuma(colC), GetLuma(colT))) {
            return (float) i - 0.5f;
        }
        currentUV += step;
    }
    return maxSteps;
}

// Search rightward along a horizontal edge line
float SearchRight(float2 uv, float maxSteps) {
    float2 step      = float2(pc.rcpFrameX, 0.0f);
    float2 currentUV = uv + step;

    [loop] for (uint i = 1; i <= (uint) maxSteps; ++i) {
        float3 colC = colorTex.SampleLevel(sPoint, currentUV, 0).rgb;
        float3 colT = colorTex.SampleLevel(sPoint, currentUV - float2(0.0f, pc.rcpFrameY), 0).rgb;
        if (!IsEdge(GetLuma(colC), GetLuma(colT))) {
            return (float) i - 0.5f;
        }
        currentUV += step;
    }
    return maxSteps;
}

// ============================================================================
// Analytical Weight Calculations
// ============================================================================

// Mathematically approximates the area under the crossing edge line
float CalculateArea(float d1, float d2, float ortho1, float ortho2) {
    float totalLength = d1 + d2 + 1.0f;
    float position    = d1;

    if (ortho1 * ortho2 < 0.0f) {
        return 0.5f * (1.0f - position / totalLength);
    } else if (ortho1 != 0.0f || ortho2 != 0.0f) {
        float midpoint = totalLength * 0.5f;
        return 0.5f * (1.0f - abs(position - midpoint) / midpoint);
    }

    return 0.5f * (1.0f - position / totalLength);
}

// ============================================================================
// Pixel/Fragment Shader Entry Point
// ============================================================================

float4 PSMain(VSOutput input): SV_Target0 {
    float2 uv          = input.uv;
    float4 centerColor = colorTex.SampleLevel(sPoint, uv, 0);

    float2 texelSize = float2(pc.rcpFrameX, pc.rcpFrameY);

    float lumaCenter = GetLuma(centerColor.rgb);
    float lumaLeft   = GetLuma(colorTex.SampleLevel(sPoint, uv - float2(texelSize.x, 0.0f), 0).rgb);
    float lumaTop    = GetLuma(colorTex.SampleLevel(sPoint, uv - float2(0.0f, texelSize.y), 0).rgb);

    bool edgeLeft = IsEdge(lumaCenter, lumaLeft);
    bool edgeTop  = IsEdge(lumaCenter, lumaTop);

    if (!edgeLeft && !edgeTop) {
        return centerColor;
    }

    float weightLeft = 0.0f;
    float weightTop  = 0.0f;
    float maxSteps   = (float) pc.maxSearchSteps;

    // 1. Process Vertical Discontinuities (Horizontal Blend Search)
    if (edgeLeft) {
        float dUp   = SearchUp(uv, maxSteps);
        float dDown = SearchDown(uv, maxSteps);

        float orthoUp   = IsEdgeAtOffset(uv + float2(0.0f, -dUp) * texelSize, float2(-texelSize.x, 0.0f)) ? -1.0f : 0.0f;
        float orthoDown = IsEdgeAtOffset(uv + float2(0.0f, dDown) * texelSize, float2(-texelSize.x, 0.0f)) ? 1.0f : 0.0f;

        weightLeft = CalculateArea(dUp, dDown, orthoUp, orthoDown);
    }

    // 2. Process Horizontal Discontinuities (Vertical Blend Search)
    if (edgeTop) {
        float dLeft  = SearchLeft(uv, maxSteps);
        float dRight = SearchRight(uv, maxSteps);

        float orthoLeft  = IsEdgeAtOffset(uv + float2(-dLeft, 0.0f) * texelSize, float2(0.0f, -texelSize.y)) ? -1.0f : 0.0f;
        float orthoRight = IsEdgeAtOffset(uv + float2(dRight, 0.0f) * texelSize, float2(0.0f, -texelSize.y)) ? 1.0f : 0.0f;

        weightTop = CalculateArea(dLeft, dRight, orthoLeft, orthoRight);
    }

    if (weightLeft <= 0.001f && weightTop <= 0.001f) {
        return centerColor;
    }

    // 3. Neighborhood Color Blending
    float4 finalColor = centerColor;

    if (weightLeft > 0.0f) {
        float4 neighborLeft = colorTex.SampleLevel(sPoint, uv - float2(texelSize.x, 0.0f), 0);
        finalColor          = lerp(finalColor, neighborLeft, weightLeft * 0.5f);
    }

    if (weightTop > 0.0f) {
        float4 neighborTop = colorTex.SampleLevel(sPoint, uv - float2(0.0f, texelSize.y), 0);
        finalColor         = lerp(finalColor, neighborTop, weightTop * 0.5f);
    }

    return finalColor;
}
