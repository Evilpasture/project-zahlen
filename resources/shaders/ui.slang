// resources/shaders/ui.slang
import common;
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

[vk::push_constant] UIObjectConstants uiObj;

// Bindless registry now comes from the `scene` parameter block (common.slang).

struct UIVSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")] UIVSOutput VSMain(uint vertexId : SV_VertexID) {
    UIVSOutput output;

    // Cast physical buffer device addresses to strongly-typed GPU pointers
    float3*           posPool  = (float3*) uiObj.posAddress;
    VertexAttributes* attrPool = (VertexAttributes*) uiObj.attrAddress;

    // Load data cleanly with array subscripting
    float3           position = posPool[vertexId];
    VertexAttributes attr     = attrPool[vertexId];

    uint uv    = attr.uv;
    uint color = attr.color;

    output.pos   = mul(uiObj.orthoMatrix, float4(position, 1.0f));
    output.uv    = LocalUnpackUV(uv);
    output.color = LocalUnpackColor(color);
    return output;
}

    [shader("fragment")] float4 PSMain(UIVSOutput input): SV_Target0 {
    float4 texSample = scene.globalTextures[uiObj.albedoIdx].Sample(scene.defaultSampler, input.uv);
    float  alpha     = texSample.a;

    if (uiObj.isSDF != 0) {
        float distance = texSample.a;
        // Screen-space derivative calculates edge sharpness dynamically at any scale
        float smoothing = max(fwidth(distance), 0.0001f);
        alpha           = smoothstep(0.5f - smoothing, 0.5f + smoothing, distance);
    }

    return float4(input.color.rgb, input.color.a * alpha);
}
