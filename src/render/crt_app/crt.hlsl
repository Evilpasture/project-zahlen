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
static const float2 MAGNET_POS        = float2(0.5, 0.5); 
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
    return frac(sin(n) * 43758.5453);
}

// ============================================================
// PIXEL SHADER
// ============================================================
float4 PSMain(VS_OUTPUT input) : SV_Target {
    float2 uv = input.uv;
    
    // 1. CRT GLASS CURVATURE
    float2 centeredUV = uv * 2.0 - 1.0;
    float2 offset = abs(centeredUV.yx) / float2(5.0, 4.0);
    centeredUV = centeredUV + centeredUV * offset * offset;
    float2 curvedUV = centeredUV * 0.5 + 0.5;

    if (any(curvedUV < 0.0) || any(curvedUV > 1.0)) {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float2 sampleUV = curvedUV;

    // 2. TIMING & GATING
    float degaussTime = fmod(pc.Time, 60.0);    
    float warmup      = saturate(pc.Time / 10.0); 

    // 3. PHYSICAL DEFORMATIONS
    if (degaussTime < 1.0 && pc.Time > 5.0) {
        float decay  = 1.0 - degaussTime;
        float shakeX = sin(pc.Time * 150.0) * decay * 0.010;
        float shakeY = cos(pc.Time * 140.0) * decay * 0.010;
        sampleUV += float2(shakeX, shakeY);
    }

    float randomSpike = pow(saturate(sin(pc.Time * 0.7)), 40.0);
    float microTremor = sin(pc.Time * 150.0) * randomSpike;

    float2 magnetDelta = curvedUV - MAGNET_POS;
    float  distToMagnet = length(magnetDelta);
    float pullPower = 0.02 / (distToMagnet + 0.01);
    float inField   = saturate(pullPower);
    inField *= inField; 

    float baseMagnetStrength = inField * (0.005 + 0.015 * microTremor) * warmup;
    float2 pullVector = magnetDelta * baseMagnetStrength * MAGNET_PULL_STR;

    float swirlPower = smoothstep(0.4, 0.0, distToMagnet) * warmup;
    float2 magnetTangent = float2(-magnetDelta.y, magnetDelta.x) / (distToMagnet + 0.001);
    float2 swirlVector = magnetTangent * swirlPower * MAGNET_SWIRL_AMT;

    float scanlineID   = floor(curvedUV.y * pc.Resolution.y * 0.5);
    float snap         = hash1(scanlineID + floor(pc.Time * 12.0));
    float interference = (snap - 0.5) * 0.0004;
    sampleUV.x += interference * (0.3 + microTremor * 10.0);

    // 4. CHROMATIC SPLIT
    float timeBlock   = floor(pc.Time * 8.0);
    float diceRoll    = IGN(float2(timeBlock, timeBlock));
    float isGlitching = step(0.99, diceRoll);
    float jitter      = isGlitching * sin(pc.Time * 250.0) * GLITCH_JITTER * warmup;

    float2 centerDist = curvedUV - 0.5;
    float  lensAb     = dot(centerDist, centerDist) * ABERRATION_SCALE;
    
    if (degaussTime < 1.0 && pc.Time > 5.0) {
        lensAb += (1.0 - degaussTime) * 0.03;
    }

    float2 pixelUnit = 1.0 / pc.Resolution;
    float2 subpixel  = pixelUnit * float2(SUBPIXEL_X, SUBPIXEL_Y);

    float2 redCoord   = sampleUV - (pullVector * 0.6) + float2( lensAb + jitter, 0.0) + subpixel + swirlVector * 1.0;
    float2 greenCoord = sampleUV - (pullVector * 1.0) + float2( jitter,          0.0)            - swirlVector * 0.5;
    float2 blueCoord  = sampleUV - (pullVector * 1.6) + float2(-lensAb + jitter, 0.0) - subpixel - swirlVector * 1.2;

    float3 color;
    color.r = shaderTexture.Sample(samplerState, redCoord).r;
    float4 centerTap = shaderTexture.Sample(samplerState, greenCoord);
    color.g = centerTap.g;
    color.b = shaderTexture.Sample(samplerState, blueCoord).b;
    float alpha = centerTap.a;

    color *= 1.35; 

    // 5. SCANLINES
    float pulse      = 1.0 + 0.15 * sin(pc.Time * 1.2); 
    float scanline   = sin(sampleUV.y * pc.Resolution.y * 3.14159265);
    scanline         = 0.5 + 0.5 * scanline;

    float brightness = dot(color, float3(0.299, 0.587, 0.114));
    float scanlineIntensity = lerp(SCANLINE_DEPTH * pulse, 0.0, brightness);
    color -= scanlineIntensity * scanline;

    // 6. APERTURE GRILLE
    float  xPos = sampleUV.x * pc.Resolution.x;
    float3 mask = (1.0 - GRILLE_DEPTH) + GRILLE_DEPTH * cos((xPos + float3(0.0, 0.333, 0.666)) * 6.28318);
    color *= mask;

    // 7. DIAGONAL BLOOM
    float3 glow  = shaderTexture.Sample(samplerState, sampleUV + pixelUnit * 1.5).rgb;
    glow        += shaderTexture.Sample(samplerState, sampleUV - pixelUnit * 1.5).rgb;
    color += glow * BLOOM_AMT * pulse;

    // 8. FINAL POST-PROCESS
    float vignette = curvedUV.x * curvedUV.y * (1.0 - curvedUV.x) * (1.0 - curvedUV.y);
    vignette       = saturate(pow(16.0 * vignette, 0.15));

    float humShadow = 1.0 - (sin(curvedUV.y * 5.0 - pc.Time * 2.0) * 0.02);
    float flicker = (0.98 + 0.02 * sin(pc.Time * 120.0)) * humShadow;

    if (degaussTime < 0.2 && pc.Time > 5.0) {
        flicker += (0.2 - degaussTime) * 2.0;
    }

    float noise = (IGN(curvedUV * pc.Resolution + pc.Time) - 0.5) * NOISE_AMP;

    color *= flicker;
    color += noise;

    float finalAlpha = lerp(1.0, alpha, vignette);

    return float4(saturate(color * vignette), finalAlpha);
}