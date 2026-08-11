// resources/shaders/ui.slang
struct VertexAttributes {
    uint normal;
    uint tangent;
    uint uv;
    uint color;
};

// --- Custom Local Unpackers ---
float2 LocalUnpackUV(uint packed) {
    return float2(f16tof32(packed & 0xFFFF), f16tof32(packed >> 16));
}

float4 LocalUnpackColor(uint packed) {
    return float4(
        float(packed & 0xFF) / 255.0f, float((packed >> 8) & 0xFF) / 255.0f, float((packed >> 16) & 0xFF) / 255.0f, float((packed >> 24) & 0xFF) / 255.0f
    );
}

struct UIObjectConstants {
    float4x4 orthoMatrix;
    uint64_t posAddress;
    uint64_t attrAddress;
    uint     albedoIdx;
    uint     isSDF;
};

[vk::push_constant] UIObjectConstants obj;

Texture2D    globalTextures[];
SamplerState defaultSampler;

struct UIVSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")] UIVSOutput VSMain(uint vertexId : SV_VertexID) {
    UIVSOutput output;

    // Cast physical buffer device addresses to strongly-typed GPU pointers
    float3*           posPool  = (float3*) obj.posAddress;
    VertexAttributes* attrPool = (VertexAttributes*) obj.attrAddress;

    // Load data cleanly with array subscripting
    float3           position = posPool[vertexId];
    VertexAttributes attr     = attrPool[vertexId];

    uint uv    = attr.uv;
    uint color = attr.color;

    output.pos   = mul(obj.orthoMatrix, float4(position, 1.0f));
    output.uv    = LocalUnpackUV(uv);
    output.color = LocalUnpackColor(color);
    return output;
}

    [shader("fragment")] float4 PSMain(UIVSOutput input): SV_Target0 {
    float4 texSample = globalTextures[obj.albedoIdx].Sample(defaultSampler, input.uv);
    float  alpha     = texSample.a;

    if (obj.isSDF != 0) {
        float distance = texSample.a;
        // Screen-space derivative calculates edge sharpness dynamically at any scale
        float smoothing = max(fwidth(distance), 0.0001f);
        alpha           = smoothstep(0.5f - smoothing, 0.5f + smoothing, distance);
    }

    return float4(input.color.rgb, input.color.a * alpha);
}
