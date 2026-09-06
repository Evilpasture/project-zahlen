// resources/shaders/ui.slang
import common;
struct UIObjectConstants {
    float4x4 orthoMatrix;
    uint64_t posAddress;
    uint64_t attrAddress;
    uint albedoIdx;
    uint isSDF;
    uint useTextureColor;
};

[vk::push_constant]
UIObjectConstants uiObj;

// Bindless registry now comes from the `scene` parameter block (common.slang).

struct UIVSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")]
UIVSOutput VSMain(uint vertexId: SV_VertexID) {
    UIVSOutput output;

    // Cast physical buffer device addresses to strongly-typed GPU pointers
    float3* posPool            = (float3*) uiObj.posAddress;
    VertexAttributes* attrPool = (VertexAttributes*) uiObj.attrAddress;

    // Load data cleanly with array subscripting
    float3 position       = posPool[vertexId];
    VertexAttributes attr = attrPool[vertexId];

    output.pos   = mul(uiObj.orthoMatrix, float4(position, 1.0f));
    output.uv    = attr.unpackedUV;
    output.color = attr.unpackedColor;
    return output;
}

[shader("fragment")]
float4 PSMain(UIVSOutput input) : SV_Target0 {
    float4 texSample = scene.globalTextures[uiObj.albedoIdx].Sample(scene.defaultSampler, input.uv);
    float alpha      = texSample.a;

    if (uiObj.isSDF != 0) {
        float distance = texSample.a;
        // Screen-space derivative calculates edge sharpness dynamically at any scale
        float smoothing = clamp(fwidth(distance) * 0.5f, 0.0001f, 0.5f);
        alpha           = smoothstep(0.5f - smoothing, 0.5f + smoothing, distance);
    }

    float3 color = uiObj.useTextureColor != 0 ? texSample.rgb * input.color.rgb : input.color.rgb;
    return float4(color, input.color.a * alpha);
}
