// ----------------------------------------------------------------------------
// Structures
// ----------------------------------------------------------------------------
[[vk::binding(0, 0)]] Texture2D cubeTexture;
[[vk::binding(1, 0)]] SamplerState cubeSampler;

struct PushConstants {
    float4x4 mvp;
};

// Use the Vulkan-specific attribute for Push Constants
[[vk::push_constant]]
PushConstants pc;

struct VSInput {
    uint vertexID : SV_VertexID;
};

struct PSInput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float3 color    : COLOR0;
};

// ----------------------------------------------------------------------------
// Data (Static Arrays)
// ----------------------------------------------------------------------------

static const float3 positions[24] = {
    // Front (+Z)
    float3(-1, -1,  1), float3( 1, -1,  1), float3( 1,  1,  1), float3(-1,  1,  1),
    // Back (-Z)
    float3( 1, -1, -1), float3(-1, -1, -1), float3(-1,  1, -1), float3( 1,  1, -1),
    // Top (+Y)
    float3(-1,  1,  1), float3( 1,  1,  1), float3( 1,  1, -1), float3(-1,  1, -1),
    // Bottom (-Y)
    float3(-1, -1, -1), float3( 1, -1, -1), float3( 1, -1,  1), float3(-1, -1,  1),
    // Right (+X)
    float3( 1, -1,  1), float3( 1, -1, -1), float3( 1,  1, -1), float3( 1,  1,  1),
    // Left (-X)
    float3(-1, -1, -1), float3(-1, -1,  1), float3(-1,  1,  1), float3(-1,  1, -1)
};

static const uint indices[36] = {
     0,  1,  2,  2,  3,  0,
     4,  5,  6,  6,  7,  4,
     8,  9, 10, 10, 11,  8,
    12, 13, 14, 14, 15, 12,
    16, 17, 18, 18, 19, 16,
    20, 21, 22, 22, 23, 20
};

static const float2 uvs[24] = {
    // Each face: BL, BR, TR, TL
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Front
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Back
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Top
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Bottom
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Right
    float2(0,1), float2(1,1), float2(1,0), float2(0,0), // Left
};

// ----------------------------------------------------------------------------
// Vertex Shader
// ----------------------------------------------------------------------------

PSInput VSMain(VSInput input) {
    PSInput output;

    uint index = indices[input.vertexID];
    float3 pos = positions[index];
    
    output.position = mul(pc.mvp, float4(pos, 1.0));
    
    // Color calculation
    output.color = normalize(pos) * 0.5f + 0.5f;

    output.uv = uvs[index];

    return output;
}

// ----------------------------------------------------------------------------
// Fragment Shader
// ----------------------------------------------------------------------------

float4 PSMain(PSInput input) : SV_Target0 {
    return cubeTexture.Sample(cubeSampler, input.uv);
}