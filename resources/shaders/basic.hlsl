struct VSInput {
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 normal   : NORMAL;
    [[vk::location(2)]] float4 tangent  : TANGENT;
    [[vk::location(3)]] float2 uv       : TEXCOORD;
    [[vk::location(4)]] float4 color    : COLOR;
};

struct VSOutput {
    float4 pos      : SV_Position;
    float3 worldPos : TEXCOORD0;
    float3 normal   : TEXCOORD1;
    float4 color    : COLOR0;
};

struct PushConstants {
    float4x4 transform;
};

[[vk::push_constant]] PushConstants pc;

VSOutput VSMain(VSInput input) {
    VSOutput output;
    
    // Calculate World Position
    float4 worldPos = mul(pc.transform, float4(input.position, 1.0f));
    output.pos = worldPos; // Assuming transform includes ViewProj
    output.worldPos = worldPos.xyz;
    
    // Normal needs to be rotated by the model matrix (ignoring scale for simplicity)
    output.normal = normalize(mul((float3x3)pc.transform, input.normal.xyz));
    output.color = input.color;
    
    return output;
}

// Simple ACES Tonemapping to make highlights look natural
float3 ACESFilm(float3 x) {
    float a = 2.51f;
    float b = 0.03f;
    float c = 2.43f;
    float d = 0.59f;
    float e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 PSMain(VSOutput input) : SV_Target {
    float3 N = normalize(input.normal);
    float3 L = normalize(float3(0.5, 1.0, 0.2)); // Directional Sun
    
    // 1. Lighting Math (Lambertian + Ambient)
    float diffuse = max(dot(N, L), 0.0);
    float3 ambient = float3(0.05, 0.06, 0.1); // Slightly blue ambient
    float3 lightColor = float3(1.0, 0.9, 0.8); // Warm sun
    
    float3 finalColor = (input.color.rgb * diffuse * lightColor) + (input.color.rgb * ambient);
    
    // 2. Tonemapping & Gamma Correction
    finalColor = ACESFilm(finalColor);
    finalColor = pow(finalColor, 1.0 / 2.2); // Convert to Gamma Space for monitor
    
    return float4(finalColor, 1.0);
}