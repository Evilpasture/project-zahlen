// ----------------------------------------------------------------------------
// Structures for Inter-Stage Communication
// ----------------------------------------------------------------------------

struct VS_OUTPUT {
    // SV_Position is the HLSL equivalent of gl_Position
    float4 position : SV_Position;
    // Custom semantic COLOR0 will be interpolated for the pixel shader
    float3 color    : COLOR0;
};

// ----------------------------------------------------------------------------
// Hardcoded Data (Static Arrays)
// ----------------------------------------------------------------------------

static const float2 positions[3] = {
    float2( 0.0, -0.5), // Top
    float2(-0.5,  0.5), // Bottom Left
    float2( 0.5,  0.5)  // Bottom Right
};

static const float3 colors[3] = {
    float3(1.0, 0.0, 0.0), // Red
    float3(0.0, 1.0, 0.0), // Green
    float3(0.0, 0.0, 1.0)  // Blue
};

// ----------------------------------------------------------------------------
// Vertex Shader
// ----------------------------------------------------------------------------

VS_OUTPUT VSMain(uint vertexID : SV_VertexID) {
    VS_OUTPUT output;

    // Use the VertexID to index into our static data
    output.position = float4(positions[vertexID], 0.0f, 1.0f);
    output.color    = colors[vertexID];

    return output;
}

// ----------------------------------------------------------------------------
// Pixel (Fragment) Shader
// ----------------------------------------------------------------------------

// SV_Target is the HLSL equivalent of layout(location = 0) out vec4
float4 PSMain(VS_OUTPUT input) : SV_Target {
    return float4(input.color, 1.0f);
}