struct PushConstants {
    float4x4 transform;
    float4x4 prevTransform; // Needed to find where the vertex was last frame
    uint textureIndex;
};
[[vk::push_constant]] PushConstants pc;

struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal   : NORMAL;
    [[vk::location(2)]] float4 tangent  : TANGENT;
    [[vk::location(3)]] float2 uv       : TEXCOORD;
    [[vk::location(4)]] float4 color    : COLOR;
};

struct VSOutput {
    float4 pos      : SV_Position;
    float4 currClip : TEXCOORD0;
    float4 prevClip : TEXCOORD1;
    float3 normal   : NORMAL;
    float2 uv       : TEXCOORD2;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    
    // 1. Current Jittered Position
    output.currClip = mul(pc.transform, float4(input.position, 1.0f));
    output.pos = output.currClip; 
    
    // 2. Previous Unjittered Position
    output.prevClip = mul(pc.prevTransform, float4(input.position, 1.0f));
    
    output.normal = mul((float3x3)pc.transform, input.normal);
    output.uv = input.uv;
    output.color = input.color;
    
    return output;
}

struct PSOutput {
    float4 color    : SV_Target0;
    float2 velocity : SV_Target1;
};

PSOutput PSMain(VSOutput input) {
    PSOutput output;
    
    // Basic lighting (replace with your PBR logic or bindless texture lookup)
    float3 N = normalize(input.normal);
    float3 L = normalize(float3(0.5, 1.0, 0.2));
    float diff = max(dot(N, L), 0.0);
    float3 finalColor = input.color.rgb * diff + input.color.rgb * 0.05;
    
    output.color = float4(finalColor, 1.0f);

    // Calculate Motion Vectors
    float2 ndcCurr = input.currClip.xy / input.currClip.w;
    float2 ndcPrev = input.prevClip.xy / input.prevClip.w;
    
    // NDC is [-1, 1], UV is [0, 1]. Vulkan Y is down, so we scale by 0.5
    output.velocity = (ndcCurr - ndcPrev) * 0.5f;
    
    return output;
}