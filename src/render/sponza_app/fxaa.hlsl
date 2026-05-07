[[vk::binding(0, 0)]] Texture2D sceneTexture;
[[vk::binding(1, 0)]] SamplerState sceneSampler;

struct VSOutput {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput vout;
    // Standard Vulkan Fullscreen Triangle
    // Generates a triangle covering [-1, 1] in NDC
    // id 0: (-1, -1), uv (0, 0)
    // id 1: ( 3, -1), uv (2, 0)
    // id 2: (-1,  3), uv (0, 2)
    vout.uv = float2((id << 1) & 2, id & 2);
    vout.pos = float4(vout.uv * float2(2.0f, 2.0f) + float2(-1.0f, -1.0f), 0.0f, 1.0f);
    
    // VULKAN Y-FLIP: 
    // If your scene is upside down, flip the Y here:
    // vout.pos.y = -vout.pos.y; 
    
    return vout;
}

float GetLuma(float3 rgb) {
    return dot(rgb, float3(0.299, 0.587, 0.114));
}

float4 PSMain(VSOutput input) : SV_TARGET {
    float2 res;
    sceneTexture.GetDimensions(res.x, res.y);
    float2 texelSize = 1.0 / res;

    float3 rgbNW = sceneTexture.Sample(sceneSampler, input.uv + float2(-1, -1) * texelSize).rgb;
    float3 rgbNE = sceneTexture.Sample(sceneSampler, input.uv + float2(1, -1) * texelSize).rgb;
    float3 rgbSW = sceneTexture.Sample(sceneSampler, input.uv + float2(-1, 1) * texelSize).rgb;
    float3 rgbSE = sceneTexture.Sample(sceneSampler, input.uv + float2(1, 1) * texelSize).rgb;
    float3 rgbM  = sceneTexture.Sample(sceneSampler, input.uv).rgb;

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

    float3 rgbA = 0.5 * (
        sceneTexture.Sample(sceneSampler, input.uv + dir * (1.0/3.0 - 0.5)).rgb +
        sceneTexture.Sample(sceneSampler, input.uv + dir * (2.0/3.0 - 0.5)).rgb);
    
    float3 rgbB = rgbA * 0.5 + 0.25 * (
        sceneTexture.Sample(sceneSampler, input.uv + dir * -0.5).rgb +
        sceneTexture.Sample(sceneSampler, input.uv + dir * 0.5).rgb);

    float lumaB = GetLuma(rgbB);
    if ((lumaB < lumaMin) || (lumaB > lumaMax)) return float4(rgbA, 1.0);
    return float4(rgbB, 1.0);
}