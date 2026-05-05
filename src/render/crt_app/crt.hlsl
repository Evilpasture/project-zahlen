// ============================================================
// VULKAN BINDINGS & STRUCTURES
// ============================================================

[[vk::binding(0, 0)]] Texture2D shaderTexture;
[[vk::binding(1, 0)]] SamplerState samplerState;

struct PushConstants {
    float  Time;
    float  Scale;
    float2 Resolution;
    float4 Background;
    float2 MousePos;
};


[[vk::push_constant]] 
PushConstants pc;

struct VS_OUTPUT {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD;
};

// ============================================================
// VERTEX SHADER (Fullscreen Triangle)
// ============================================================
VS_OUTPUT VSMain(uint vertexID : SV_VertexID) {
    VS_OUTPUT output;
    // Generates a single triangle covering the entire screen
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(output.uv * 2.0f - 1.0f, 0.0f, 1.0f);
    return output;
}


// ============================================================
// TUNING CONSTANTS
// ============================================================
// static const float2 MAGNET_POS        = float2(0.5, 0.5); // unused! but can be reused for a static magnet
static const float MAGNET_PULL_STR    = 1.0; 
static const float MAGNET_SWIRL_AMT   = 0.005; 
static const float ABERRATION_SCALE   = 0.0018;
static const float SUBPIXEL_X         = 0.45; 
static const float SUBPIXEL_Y         = 0.15; 
static const float GLITCH_JITTER      = 0.002;
static const float GRILLE_DEPTH       = 0.20;
static const float SCANLINE_DEPTH     = 0.08;
static const float NOISE_AMP          = 0.04;
static const float BLOOM_AMT          = 0.15;

// ============================================================
// UTILITIES
// ============================================================
float IGN(float2 uv) {
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(uv, magic.xy)));
}

float hash1(float n) {
    // The fractional part of (n * Golden Ratio)
    // 0.61803398875 is the fractional part of Phi
    return frac(n * 0.61803398875);
}

// ============================================================
// PIXEL SHADER
// ============================================================
float4 PSMain(VS_OUTPUT input) : SV_Target {
    float2 uv = input.uv; // Raw [0,1] linear UV, matches OS cursor space exactly

    // =========================================================
    // 1. MAGNET PHYSICS (computed in LINEAR UV space)
    //    Must happen before curvature so the pull origin matches
    //    the actual OS cursor position with no barrel-distortion offset.
    // =========================================================
    float2 magnetDelta  = uv - pc.MousePos;
    float  distToMagnet = length(magnetDelta);

    float randomSpike = pow(saturate(sin(pc.Time * 0.7)), 40.0);
    float microTremor = sin(pc.Time * 150.0) * randomSpike;
    float warmup      = saturate(pc.Time / 10.0);

    float pullPower = 0.02 / (distToMagnet + 0.01);
    float inField   = saturate(pullPower);
    inField *= inField;

    float baseMagnetStrength = inField * (0.005 + 0.015 * microTremor) * warmup;
    float2 pullVector = magnetDelta * baseMagnetStrength * MAGNET_PULL_STR;

    // Swirl tangent is also computed in linear space for the same reason
    float swirlPower    = smoothstep(0.4, 0.0, distToMagnet) * warmup;
    float2 magnetTangent = float2(-magnetDelta.y, magnetDelta.x) / (distToMagnet + 0.001);
    float2 swirlVector  = magnetTangent * swirlPower * MAGNET_SWIRL_AMT;

    // =========================================================
    // 2. CRT GLASS CURVATURE (visual warp for sampling only)
    //    Applied AFTER magnet physics so the barrel distortion
    //    does not shift the perceived cursor position.
    // =========================================================
    float2 centeredUV = uv * 2.0 - 1.0;
    float2 offset     = abs(centeredUV.yx) / float2(5.0, 4.0);
    centeredUV        = centeredUV + centeredUV * offset * offset;
    float2 curvedUV   = centeredUV * 0.5 + 0.5;

    // Discard pixels that fall outside the curved screen boundary (the black bezel area)
    if (any(curvedUV < 0.0) || any(curvedUV > 1.0))
        return float4(0.0, 0.0, 0.0, 1.0);

    float2 sampleUV = curvedUV;

    // =========================================================
    // 3. TIMING & GATING
    // =========================================================
    float degaussTime = fmod(pc.Time, 60.0);

    // =========================================================
    // 4. PHYSICAL DEFORMATIONS
    //    Periodic degauss shake and per-scanline interference
    // =========================================================
    if (degaussTime < 1.0 && pc.Time > 5.0) {
        float decay  = 1.0 - degaussTime;
        float shakeX = sin(pc.Time * 150.0) * decay * 0.010;
        float shakeY = cos(pc.Time * 140.0) * decay * 0.010;
        sampleUV += float2(shakeX, shakeY);
    }

    // Per-scanline horizontal jitter — simulates analog sync noise
    float scanlineID   = floor(curvedUV.y * pc.Resolution.y * 0.5);
    float snap         = hash1(scanlineID + floor(pc.Time * 12.0));
    float interference = (snap - 0.5) * 0.0004;
    sampleUV.x += interference * (0.3 + microTremor * 10.0);

    // =========================================================
    // 5. CHROMATIC ABERRATION + GLITCH JITTER
    //    Lens aberration grows quadratically from center.
    //    Rare glitch frames add a horizontal spike across all channels.
    // =========================================================
    float timeBlock   = floor(pc.Time * 8.0);
    float diceRoll    = IGN(float2(timeBlock, timeBlock));
    float isGlitching = step(0.99, diceRoll);          // fires ~1% of 8Hz ticks
    float jitter      = isGlitching * sin(pc.Time * 250.0) * GLITCH_JITTER * warmup;

    float2 centerDist = curvedUV - 0.5;
    float  lensAb     = dot(centerDist, centerDist) * ABERRATION_SCALE;

    if (degaussTime < 1.0 && pc.Time > 5.0)
        lensAb += (1.0 - degaussTime) * 0.03;  // extra fringe during degauss shake

    float2 pixelUnit = 1.0 / pc.Resolution;
    float2 subpixel  = pixelUnit * float2(SUBPIXEL_X, SUBPIXEL_Y);

    // Each channel is pulled a different amount — red least, blue most.
    // pullVector and swirlVector were computed in linear space, but since
    // they are small delta offsets applied to curvedUV they remain valid here.
    float2 redCoord   = sampleUV - (pullVector * 0.6) + float2( lensAb + jitter, 0.0) + subpixel + swirlVector * 1.0;
    float2 greenCoord = sampleUV - (pullVector * 1.0) + float2( jitter,          0.0)             - swirlVector * 0.5;
    float2 blueCoord  = sampleUV - (pullVector * 1.6) + float2(-lensAb + jitter, 0.0) - subpixel  - swirlVector * 1.2;

    float3 color;
    color.r = shaderTexture.Sample(samplerState, redCoord).r;
    float4 centerTap = shaderTexture.Sample(samplerState, greenCoord);
    color.g = centerTap.g;
    color.b = shaderTexture.Sample(samplerState, blueCoord).b;
    float alpha = centerTap.a;

    color *= 1.35; // Compensate for energy lost to grille/scanline darkening

    // =========================================================
    // 6. SCANLINES
    //    Sine-wave darkening between phosphor rows.
    //    Intensity is brightness-adaptive: bright pixels lose less.
    // =========================================================
    float pulse    = 1.0 + 0.15 * sin(pc.Time * 1.2);
    float scanline = 0.5 + 0.5 * sin(sampleUV.y * pc.Resolution.y * 3.14159265);

    float brightness        = dot(color, float3(0.299, 0.587, 0.114));
    float scanlineIntensity = lerp(SCANLINE_DEPTH * pulse, 0.0, brightness);
    color -= scanlineIntensity * scanline;

    // =========================================================
    // 7. APERTURE GRILLE
    //    Vertical RGB stripe mask mimicking a Trinitron-style grille.
    //    Three cosine phases at 0°/120°/240° = one RGB triad per pixel.
    // =========================================================
    float  xPos = sampleUV.x * pc.Resolution.x;
    float3 mask = (1.0 - GRILLE_DEPTH) + GRILLE_DEPTH * cos((xPos + float3(0.0, 0.333, 0.666)) * 6.28318);
    color *= mask;

    // =========================================================
    // 8. DIAGONAL BLOOM
    //    Two diagonal taps add a soft glow around bright areas,
    //    simulating phosphor bleed on the glass face.
    // =========================================================
    float3 glow  = shaderTexture.Sample(samplerState, sampleUV + pixelUnit * 1.5).rgb;
    glow        += shaderTexture.Sample(samplerState, sampleUV - pixelUnit * 1.5).rgb;
    color += glow * BLOOM_AMT * pulse;

    // =========================================================
    // 9. FINAL POST-PROCESS
    //    Vignette, hum bar, flicker, and film grain applied last
    //    so they sit on top of all phosphor simulation.
    // =========================================================

    // Vignette: quartic falloff from center, softened with pow
    float vignette = curvedUV.x * curvedUV.y * (1.0 - curvedUV.x) * (1.0 - curvedUV.y);
    vignette       = saturate(pow(16.0 * vignette, 0.15));

    // Hum bar: slow vertical shadow traveling downward, simulates 50/60Hz ripple
    float humShadow = 1.0 - (sin(curvedUV.y * 5.0 - pc.Time * 2.0) * 0.02);
    float flicker   = (0.98 + 0.02 * sin(pc.Time * 120.0)) * humShadow;

    if (degaussTime < 0.2 && pc.Time > 5.0)
        flicker += (0.2 - degaussTime) * 2.0; // flash on degauss trigger

    // Grain: spatially and temporally varying, scaled by NOISE_AMP
    float noise = (IGN(curvedUV * pc.Resolution + pc.Time) - 0.5) * NOISE_AMP;

    color *= flicker;
    color += noise;

    // Alpha feathers to transparent at the curved screen edges
    float finalAlpha = lerp(1.0, alpha, vignette);

    return float4(saturate(color * vignette), finalAlpha);
}