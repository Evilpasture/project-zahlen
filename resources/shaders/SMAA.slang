// resources/shaders/SMAA.slang

// ============================================================================
// Configuration & Presets
// ============================================================================
#ifndef SMAA_THRESHOLD
#define SMAA_THRESHOLD 0.1f
#endif

#ifndef SMAA_MAX_SEARCH_STEPS
#define SMAA_MAX_SEARCH_STEPS 16
#endif

#ifndef SMAA_MAX_SEARCH_STEPS_DIAG
#define SMAA_MAX_SEARCH_STEPS_DIAG 8
#endif

#ifndef SMAA_CORNER_ROUNDING
#define SMAA_CORNER_ROUNDING 25
#endif

#ifndef SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR
#define SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR 2.0f
#endif

static const int SMAA_AREATEX_MAX_DISTANCE      = 16;
static const int SMAA_AREATEX_MAX_DISTANCE_DIAG = 20;
static const float2 SMAA_AREATEX_PIXEL_SIZE     = float2(1.0f / 160.0f, 1.0f / 560.0f);
static const float SMAA_AREATEX_SUBTEX_SIZE     = 1.0f / 7.0f;
static const float2 SMAA_SEARCHTEX_SIZE         = float2(66.0f, 33.0f);
static const float2 SMAA_SEARCHTEX_PACKED_SIZE  = float2(64.0f, 16.0f);
static const float SMAA_CORNER_ROUNDING_NORM    = float(SMAA_CORNER_ROUNDING) / 100.0f;

// ============================================================================
// Host-Reflected Parameters
// ============================================================================
struct PushConstants {
    // NOTE FOR HOST-SIDE INTEGRATION:
    // If additional parameters are appended here, ensure they are
    // aligned to 16-byte boundaries (using alignas(16) on the C++ side)
    // to match Vulkan std430 layout requirements.
    float4 rtMetrics; // x: 1/w, y: 1/h, z: w, w: h
};

[vk::push_constant]
PushConstants pc;

#define SMAA_RT_METRICS pc.rtMetrics

#if defined(EDGE_PASS)
Texture2D colorTex;
SamplerState pointSampler;
#endif

#if defined(WEIGHT_PASS)
Texture2D edgesTex;
Texture2D areaTex;
Texture2D searchTex;
SamplerState linearSampler;
SamplerState pointSampler;
#endif

#if defined(BLEND_PASS)
Texture2D colorTex;
Texture2D blendTex;
SamplerState linearSampler;
SamplerState pointSampler;
#endif

// ============================================================================
// Core Utility Functions
// ============================================================================

void Movc(bool2 cond, inout float2 variable, float2 value) {
    if (cond.x)
        variable.x = value.x;
    if (cond.y)
        variable.y = value.y;
}

void Movc(bool4 cond, inout float4 variable, float4 value) {
    Movc(cond.xy, variable.xy, value.xy);
    Movc(cond.zw, variable.zw, value.zw);
}

float2 DecodeDiagBilinearAccess(float2 e) {
    e.r = e.r * abs(5.0f * e.r - 5.0f * 0.75f);
    return round(e);
}

float4 DecodeDiagBilinearAccess(float4 e) {
    e.rb = e.rb * abs(5.0f * e.rb - 5.0f * 0.75f);
    return round(e);
}

float2 SearchDiag1(Texture2D edgesTex, SamplerState smp, float2 texcoord, float2 dir, out float2 e, float4 rtMetrics) {
    float4 coord = float4(texcoord, -1.0f, 1.0f);
    float3 t     = float3(rtMetrics.xy, 1.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS_DIAG; ++i) {
        if (coord.z >= float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) || coord.w <= 0.9f) {
            break;
        }
        coord.xyz = t * float3(dir, 1.0f) + coord.xyz;
        e         = edgesTex.SampleLevel(smp, coord.xy, 0.0f).rg;
        coord.w   = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 SearchDiag2(Texture2D edgesTex, SamplerState smp, float2 texcoord, float2 dir, out float2 e, float4 rtMetrics) {
    float4 coord = float4(texcoord, -1.0f, 1.0f);
    coord.x += 0.25f * rtMetrics.x;
    float3 t = float3(rtMetrics.xy, 1.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS_DIAG; ++i) {
        if (coord.z >= float(SMAA_MAX_SEARCH_STEPS_DIAG - 1) || coord.w <= 0.9f) {
            break;
        }
        coord.xyz = t * float3(dir, 1.0f) + coord.xyz;
        e         = edgesTex.SampleLevel(smp, coord.xy, 0.0f).rg;
        e         = DecodeDiagBilinearAccess(e);
        coord.w   = dot(e, float2(0.5f, 0.5f));
    }
    return coord.zw;
}

float2 AreaDiag(Texture2D areaTex, SamplerState smp, float2 dist, float2 e, float offset) {
    float2 texcoord = float2(SMAA_AREATEX_MAX_DISTANCE_DIAG, SMAA_AREATEX_MAX_DISTANCE_DIAG) * e + dist;
    texcoord        = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5f * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.x += 0.5f;
    texcoord.y += SMAA_AREATEX_SUBTEX_SIZE * offset;
    return areaTex.SampleLevel(smp, texcoord, 0.0f).rg; // FIXED: Returns raw .rg to support downstream .gr swizzling
}

float AreaDiagInternal(float2 p1, float2 p2, float2 p) {
    if (p1.x == p2.x && p1.y == p2.y)
        return 1.0f;
    float xm         = (p1.x + p2.x) * 0.5f;
    float ym         = (p1.y + p2.y) * 0.5f;
    float a          = p2.y - p1.y;
    float b          = p1.x - p2.x;
    float const_part = -a * xm - b * ym;

    float count                    = 0.0f;
    static const int samples       = 30;
    static const float inv_samples = 1.0f / float(samples - 1);

    [unroll]
    for (int x = 0; x < samples; ++x) {
        float px   = p.x + float(x) * inv_samples;
        float a_px = a * px;
        [unroll]
        for (int y = 0; y < samples; ++y) {
            float py = p.y + float(y) * inv_samples;
            if ((a_px + b * py + const_part) > 0.0f) {
                count += 1.0f;
            }
        }
    }
    return count / float(samples * samples);
}

static const int2 EdgesDiag[16] = { int2(0, 0), int2(1, 0), int2(0, 2), int2(1, 2), int2(2, 0), int2(3, 0), int2(2, 2), int2(3, 2),
                                    int2(0, 1), int2(1, 1), int2(0, 3), int2(1, 3), int2(2, 1), int2(3, 1), int2(2, 3), int2(3, 3) };

float2 AreaDiagHelper(int pattern, float2 p1, float2 p2, float left, float2 offset) {
    int e1   = EdgesDiag[pattern].x;
    int e2   = EdgesDiag[pattern].y;
    p1       = (e1 > 0) ? p1 + offset : p1;
    p2       = (e2 > 0) ? p2 + offset : p2;
    float a1 = AreaDiagInternal(p1, p2, float2(1.0f, 0.0f) + float2(left, left));
    float a2 = AreaDiagInternal(p1, p2, float2(1.0f, 1.0f) + float2(left, left));
    return float2(1.0f - a1, a2);
}

float2 CalculateDiagWeights(Texture2D edgesTex, Texture2D areaTex, SamplerState smp, float2 texcoord, float2 e, float4 subsampleIndices, float4 rtMetrics) {
    float2 weights = float2(0.0f, 0.0f);

    float4 d;
    float2 end;
    if (e.r > 0.0f) {
        d.xz = SearchDiag1(edgesTex, smp, texcoord, float2(-1.0f, 1.0f), end, rtMetrics);
        d.x += float(end.y > 0.9f);
    } else {
        d.xz = float2(0.0f, 0.0f);
    }
    d.yw = SearchDiag1(edgesTex, smp, texcoord, float2(1.0f, -1.0f), end, rtMetrics);

    [branch]
    if (d.x + d.y > 2.0f) {
        float4 coords = float4(-d.x + 0.25f, d.x, d.y, -d.y - 0.25f) * rtMetrics.xyxy + texcoord.xyxy;
        float4 c;
        c.xy   = edgesTex.SampleLevel(smp, coords.xy, 0.0f, int2(-1, 0)).rg;
        c.zw   = edgesTex.SampleLevel(smp, coords.zw, 0.0f, int2(1, 0)).rg;
        c.yxwz = DecodeDiagBilinearAccess(c.xyzw);

        float2 cc = float2(2.0f, 2.0f) * c.xz + c.yw;

        Movc(bool2(step(0.9f, d.zw)), cc, float2(0.0f, 0.0f));

        weights += AreaDiag(areaTex, smp, d.xy, cc, subsampleIndices.z);
    }

    d.xz = SearchDiag2(edgesTex, smp, texcoord, float2(-1.0f, -1.0f), end, rtMetrics);
    if (edgesTex.SampleLevel(smp, texcoord, 0.0f, int2(1, 0)).r > 0.0f) {
        d.yw = SearchDiag2(edgesTex, smp, texcoord, float2(1.0f, 1.0f), end, rtMetrics);
        d.y += float(end.y > 0.9f);
    } else {
        d.yw = float2(0.0f, 0.0f);
    }

    [branch]
    if (d.x + d.y > 2.0f) {
        float4 coords = float4(-d.x, -d.x, d.y, d.y) * rtMetrics.xyxy + texcoord.xyxy;
        float4 c;
        c.x       = edgesTex.SampleLevel(smp, coords.xy, 0.0f, int2(-1, 0)).g;
        c.y       = edgesTex.SampleLevel(smp, coords.xy, 0.0f, int2(0, -1)).r;
        c.zw      = edgesTex.SampleLevel(smp, coords.zw, 0.0f, int2(1, 0)).gr;
        float2 cc = float2(2.0f, 2.0f) * c.xz + c.yw;

        Movc(bool2(step(0.9f, d.zw)), cc, float2(0.0f, 0.0f));

        weights += AreaDiag(areaTex, smp, d.xy, cc, subsampleIndices.w).gr;
    }

    return weights;
}

float SearchLength(Texture2D searchTex, SamplerState smp, float2 e, float offset) {
    float2 scale = SMAA_SEARCHTEX_SIZE * float2(0.5f, -1.0f);
    float2 bias  = SMAA_SEARCHTEX_SIZE * float2(offset, 1.0f);

    scale += float2(-1.0f, 1.0f);
    bias += float2(0.5f, -0.5f);

    scale *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;
    bias *= 1.0f / SMAA_SEARCHTEX_PACKED_SIZE;

    return searchTex.SampleLevel(smp, scale * e + bias, 0.0f).r;
}

float SearchXLeft(Texture2D edgesTex, Texture2D searchTex, SamplerState smp, float2 texcoord, float end, float4 rtMetrics) {
    float2 e = float2(0.0f, 1.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; ++i) {
        if (texcoord.x <= end || e.g <= 0.8281f || e.r != 0.0f) {
            break;
        }
        e = edgesTex.SampleLevel(smp, texcoord, 0.0f).rg;
        texcoord -= float2(2.0f * rtMetrics.x, 0.0f);
    }

    float offset = -(255.0f / 127.0f) * SearchLength(searchTex, smp, e, 0.0f) + 3.25f;
    return rtMetrics.x * offset + texcoord.x;
}

float SearchXRight(Texture2D edgesTex, Texture2D searchTex, SamplerState smp, float2 texcoord, float end, float4 rtMetrics) {
    float2 e = float2(0.0f, 1.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; ++i) {
        if (texcoord.x >= end || e.g <= 0.8281f || e.r != 0.0f) {
            break;
        }
        e = edgesTex.SampleLevel(smp, texcoord, 0.0f).rg;
        texcoord += float2(2.0f * rtMetrics.x, 0.0f);
    }
    float offset = -(255.0f / 127.0f) * SearchLength(searchTex, smp, e, 0.5f) + 3.25f;
    return -rtMetrics.x * offset + texcoord.x;
}

float SearchYUp(Texture2D edgesTex, Texture2D searchTex, SamplerState smp, float2 texcoord, float end, float4 rtMetrics) {
    float2 e = float2(1.0f, 0.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; ++i) {
        if (texcoord.y <= end || e.r <= 0.8281f || e.g != 0.0f) {
            break;
        }
        e = edgesTex.SampleLevel(smp, texcoord, 0.0f).rg;
        texcoord -= float2(0.0f, 2.0f * rtMetrics.y);
    }
    float offset = -(255.0f / 127.0f) * SearchLength(searchTex, smp, e.gr, 0.0f) + 3.25f;
    return rtMetrics.y * offset + texcoord.y;
}

float SearchYDown(Texture2D edgesTex, Texture2D searchTex, SamplerState smp, float2 texcoord, float end, float4 rtMetrics) {
    float2 e = float2(1.0f, 0.0f);

    [unroll]
    for (int i = 0; i < SMAA_MAX_SEARCH_STEPS; ++i) {
        if (texcoord.y >= end || e.r <= 0.8281f || e.g != 0.0f) {
            break;
        }
        e = edgesTex.SampleLevel(smp, texcoord, 0.0f).rg;
        texcoord += float2(0.0f, 2.0f * rtMetrics.y);
    }
    float offset = -(255.0f / 127.0f) * SearchLength(searchTex, smp, e.gr, 0.5f) + 3.25f;
    return -rtMetrics.y * offset + texcoord.y;
}

float2 Area(Texture2D areaTex, SamplerState smp, float2 dist, float e1, float e2, float offset) {
    float2 texcoord = float2(SMAA_AREATEX_MAX_DISTANCE, SMAA_AREATEX_MAX_DISTANCE) * round(4.0f * float2(e1, e2)) + dist;
    texcoord        = SMAA_AREATEX_PIXEL_SIZE * texcoord + 0.5f * SMAA_AREATEX_PIXEL_SIZE;
    texcoord.y      = SMAA_AREATEX_SUBTEX_SIZE * offset + texcoord.y;
    return areaTex.SampleLevel(smp, texcoord, 0.0f).rg;
}

void DetectHorizontalCornerPattern(Texture2D edgesTex, SamplerState smp, inout float2 weights, float4 texcoord, float2 d, float4 rtMetrics) {
#if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding  = (1.0f - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * edgesTex.SampleLevel(smp, texcoord.xy + float2(0.0f, 1.0f) * rtMetrics.xy, 0.0f).r;
    factor.y -= rounding.x * edgesTex.SampleLevel(smp, texcoord.xy + float2(0.0f, -2.0f) * rtMetrics.xy, 0.0f).r;
    factor.x -= rounding.y * edgesTex.SampleLevel(smp, texcoord.zw + float2(1.0f, 1.0f) * rtMetrics.xy, 0.0f).r;
    factor.y -= rounding.y * edgesTex.SampleLevel(smp, texcoord.zw + float2(1.0f, -2.0f) * rtMetrics.xy, 0.0f).r;

    weights *= saturate(factor);
#endif
}

void DetectVerticalCornerPattern(Texture2D edgesTex, SamplerState smp, inout float2 weights, float4 texcoord, float2 d, float4 rtMetrics) {
#if !defined(SMAA_DISABLE_CORNER_DETECTION)
    float2 leftRight = step(d.xy, d.yx);
    float2 rounding  = (1.0f - SMAA_CORNER_ROUNDING_NORM) * leftRight;

    rounding /= leftRight.x + leftRight.y;

    float2 factor = float2(1.0f, 1.0f);
    factor.x -= rounding.x * edgesTex.SampleLevel(smp, texcoord.xy + float2(1.0f, 0.0f) * rtMetrics.xy, 0.0f).g;
    factor.y -= rounding.x * edgesTex.SampleLevel(smp, texcoord.xy + float2(-2.0f, 0.0f) * rtMetrics.xy, 0.0f).g;
    factor.x -= rounding.y * edgesTex.SampleLevel(smp, texcoord.zw + float2(1.0f, 1.0f) * rtMetrics.xy, 0.0f).g;
    factor.y -= rounding.y * edgesTex.SampleLevel(smp, texcoord.zw + float2(-2.0f, 1.0f) * rtMetrics.xy, 0.0f).g;

    weights *= saturate(factor);
#endif
}

public float2 LumaEdgeDetectionPS(float2 texcoord, float4 offset[3], Texture2D colorTex, SamplerState smp, float4 rtMetrics) {
    float2 threshold = float2(SMAA_THRESHOLD, SMAA_THRESHOLD);

    float3 weights = float3(0.2126f, 0.7152f, 0.0722f);
    float L        = dot(colorTex.SampleLevel(smp, texcoord, 0.0f).rgb, weights);

    float Lleft = dot(colorTex.SampleLevel(smp, offset[0].xy, 0.0f).rgb, weights);
    float Ltop  = dot(colorTex.SampleLevel(smp, offset[0].zw, 0.0f).rgb, weights);

    float4 delta;
    delta.xy     = abs(L - float2(Lleft, Ltop));
    float2 edges = step(threshold, delta.xy);

    // FIXED: Removed the fragment discard branch. Returning clean zero vector
    // prevents warp divergence and early-z bubbles on tiled mobile architectures.

    float Lright  = dot(colorTex.SampleLevel(smp, offset[1].xy, 0.0f).rgb, weights);
    float Lbottom = dot(colorTex.SampleLevel(smp, offset[1].zw, 0.0f).rgb, weights);
    delta.zw      = abs(L - float2(Lright, Lbottom));

    float2 maxDelta = max(delta.xy, delta.zw);

    float Lleftleft = dot(colorTex.SampleLevel(smp, offset[2].xy, 0.0f).rgb, weights);
    float Ltoptop   = dot(colorTex.SampleLevel(smp, offset[2].zw, 0.0f).rgb, weights);
    delta.zw        = abs(float2(Lleft, Ltop) - float2(Lleftleft, Ltoptop));

    maxDelta         = max(maxDelta.xy, delta.zw);
    float finalDelta = max(maxDelta.x, maxDelta.y);

    edges.xy *= step(finalDelta, SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR * delta.xy);

    return edges;
}

public float4 BlendingWeightCalculationPS(float2 texcoord, float2 pixcoord, float4 offset[3], Texture2D edgesTex, Texture2D areaTex, Texture2D searchTex,
                                          SamplerState smp, float4 subsampleIndices, float4 rtMetrics) {
    float4 weights = float4(0.0f, 0.0f, 0.0f, 0.0f);
    float2 e       = edgesTex.SampleLevel(smp, texcoord, 0.0f).rg;

    [branch]
    if (e.g > 0.0f) { // Edge at north
#if !defined(SMAA_DISABLE_DIAG_DETECTION)
        weights.rg = CalculateDiagWeights(edgesTex, areaTex, smp, texcoord, e, subsampleIndices, rtMetrics);

        [branch]
        if (weights.r == -weights.g) {
#endif

            float2 d;

            float3 coords;
            coords.x = SearchXLeft(edgesTex, searchTex, smp, offset[0].xy, offset[2].x, rtMetrics);
            coords.y = offset[1].y;
            d.x      = coords.x;

            float e1 = edgesTex.SampleLevel(smp, coords.xy, 0.0f).r;

            coords.z = SearchXRight(edgesTex, searchTex, smp, offset[0].zw, offset[2].y, rtMetrics);
            d.y      = coords.z;

            d = abs(round(rtMetrics.zz * d - pixcoord.xx));

            float2 sqrt_d = sqrt(d);

            float e2 = edgesTex.SampleLevel(smp, coords.zy, 0.0f, int2(1, 0)).r;

            weights.rg = Area(areaTex, smp, sqrt_d, e1, e2, subsampleIndices.y);

            coords.y = texcoord.y;
            DetectHorizontalCornerPattern(edgesTex, smp, weights.rg, coords.xyzy, d, rtMetrics);

#if !defined(SMAA_DISABLE_DIAG_DETECTION)
        } else {
            e.r = 0.0f;
        }
#endif
    }

    [branch]
    if (e.r > 0.0f) { // Edge at west
        float2 d;

        // Find the distance to the top:
        float3 coords;
        coords.y = SearchYUp(edgesTex, searchTex, smp, offset[1].xy, offset[2].z, rtMetrics);
        coords.x = offset[0].x;
        d.x      = coords.y;

        // Fetch the top crossing edges:
        float e1 = edgesTex.SampleLevel(smp, coords.xy, 0.0f).g;

        // Find the distance to the bottom:
        coords.z = SearchYDown(edgesTex, searchTex, smp, offset[1].zw, offset[2].w, rtMetrics);
        d.y      = coords.z;

        // We want the distances to be in pixel units:
        d = abs(round(rtMetrics.ww * d - pixcoord.yy));

        // SMAAArea below needs a sqrt, as the areas texture is compressed
        // quadratically:
        float2 sqrt_d = sqrt(d);

        // Fetch the bottom crossing edges:
        float e2 = edgesTex.SampleLevel(smp, coords.xz, 0.0f, int2(0, 1)).g;

        // Get the area for this direction:
        weights.ba = Area(areaTex, smp, sqrt_d, e1, e2, subsampleIndices.x);

        // Fix corners:
        coords.x = texcoord.x;
        DetectVerticalCornerPattern(edgesTex, smp, weights.ba, coords.xyxz, d, rtMetrics);
    }

    return weights;
}

public float4 NeighborhoodBlendingPS(float2 texcoord, float4 offset, Texture2D colorTex, Texture2D blendTex, SamplerState smp, float4 rtMetrics) {
    float4 a;
    a.x  = blendTex.SampleLevel(smp, offset.xy, 0.0f).a;
    a.y  = blendTex.SampleLevel(smp, offset.zw, 0.0f).g;
    a.wz = blendTex.SampleLevel(smp, texcoord, 0.0f).xz;

    [branch]
    if (dot(a, float4(1.0f, 1.0f, 1.0f, 1.0f)) < 1e-5f) {
        return colorTex.SampleLevel(smp, texcoord, 0.0f);
    } else {
        bool h = max(a.x, a.z) > max(a.y, a.w);

        float4 blendingOffset = float4(0.0f, a.y, 0.0f, a.w);
        float2 blendingWeight = a.yw;
        Movc(bool4(h, h, h, h), blendingOffset, float4(a.x, 0.0f, a.z, 0.0f));
        Movc(bool2(h, h), blendingWeight, a.xz);
        blendingWeight /= dot(blendingWeight, float2(1.0f, 1.0f));

        float4 blendingCoord = blendingOffset * float4(rtMetrics.xy, -rtMetrics.xy) + texcoord.xyxy;

        float4 color = blendingWeight.x * colorTex.SampleLevel(smp, blendingCoord.xy, 0.0f);
        color += blendingWeight.y * colorTex.SampleLevel(smp, blendingCoord.zw, 0.0f);

        return color;
    }
}

// ============================================================================
// PASS 1: Edge Detection
// ============================================================================
#if defined(EDGE_PASS)
struct EdgeVSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 offset[3] : TEXCOORD1;
};

[shader("vertex")]
EdgeVSOutput SmaaEdgeVS(uint vertexID: SV_VertexID) {
    EdgeVSOutput output;
    output.uv       = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

    output.offset[0] = SMAA_RT_METRICS.xyxy * float4(-1.0f, 0.0f, 0.0f, -1.0f) + output.uv.xyxy;
    output.offset[1] = SMAA_RT_METRICS.xyxy * float4(1.0f, 0.0f, 0.0f, 1.0f) + output.uv.xyxy;
    output.offset[2] = SMAA_RT_METRICS.xyxy * float4(-2.0f, 0.0f, 0.0f, -2.0f) + output.uv.xyxy;
    return output;
}

[shader("fragment")]
float4 SmaaEdgePS(EdgeVSOutput input) : SV_Target0 {
    return float4(LumaEdgeDetectionPS(input.uv, input.offset, colorTex, pointSampler, SMAA_RT_METRICS), 0.0f, 0.0f);
}
#endif

// ============================================================================
// PASS 2: Blending Weight Calculation
// ============================================================================
#if defined(WEIGHT_PASS)
struct WeightVSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 pixcoord : TEXCOORD1;
    float4 offset[3] : TEXCOORD2;
};

[shader("vertex")]
WeightVSOutput SmaaWeightVS(uint vertexID: SV_VertexID) {
    WeightVSOutput output;
    output.uv       = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

    output.pixcoord  = output.uv * SMAA_RT_METRICS.zw;
    output.offset[0] = SMAA_RT_METRICS.xyxy * float4(-0.25f, -0.125f, 1.25f, -0.125f) + output.uv.xyxy;
    output.offset[1] = SMAA_RT_METRICS.xyxy * float4(-0.125f, -0.25f, -0.125f, 1.25f) + output.uv.xyxy;
    output.offset[2] = SMAA_RT_METRICS.xxyy * (float4(-2.0f, 2.0f, -2.0f, 2.0f) * float(SMAA_MAX_SEARCH_STEPS)),
    float4(output.offset[0].xz, output.offset[1].yw);
    return output;
}

[shader("fragment")]
float4 SmaaWeightPS(WeightVSOutput input) : SV_Target0 {
    return BlendingWeightCalculationPS(input.uv, input.pixcoord, input.offset, edgesTex, areaTex, searchTex, linearSampler, float4(0.0f, 0.0f, 0.0f, 0.0f),
                                       SMAA_RT_METRICS);
}
#endif

// ============================================================================
// PASS 3: Neighborhood Blending
// ============================================================================
#if defined(BLEND_PASS)
struct BlendVSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float4 offset : TEXCOORD1;
};

[shader("vertex")]
BlendVSOutput SmaaBlendVS(uint vertexID: SV_VertexID) {
    BlendVSOutput output;
    output.uv       = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv.x * 2.0f - 1.0f, output.uv.y * 2.0f - 1.0f, 0.0f, 1.0f);

    output.offset = SMAA_RT_METRICS.xyxy * float4(1.0f, 0.0f, 0.0f, 1.0f) + output.uv.xyxy;
    return output;
}

[shader("fragment")]
float4 SmaaBlendPS(BlendVSOutput input) : SV_Target0 {
    return NeighborhoodBlendingPS(input.uv, input.offset, colorTex, blendTex, linearSampler, SMAA_RT_METRICS);
}
#endif
