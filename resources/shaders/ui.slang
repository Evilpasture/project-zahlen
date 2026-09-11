// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Standalone UI shader. Does not import `common`: a 2D overlay must not
// bind GlobalSceneRegistry (IBL, shadows, volumetric, instance buffers).
import instance_data;
import vertex_format;

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

/// Sampler + bindless texture array. Member order is the binding table.
struct UIRegistry {
    SamplerState sampler;  // 0
    Texture2D textures[];  // 1
};

ParameterBlock<UIRegistry> ui;

struct UIVSOutput {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")]
UIVSOutput VSMain(uint vertexId: SV_VertexID) {
    UIVSOutput output;

    float3* posPool            = (float3*) uiObj.posAddress;
    VertexAttributes* attrPool = (VertexAttributes*) uiObj.attrAddress;

    float3 position       = posPool[vertexId];
    VertexAttributes attr = attrPool[vertexId];

    output.pos   = mul(uiObj.orthoMatrix, float4(position, 1.0f));
    output.uv    = attr.unpackedUV;
    output.color = attr.unpackedColor;
    return output;
}

[shader("fragment")]
float4 PSMain(UIVSOutput input) : SV_Target0 {
    float4 texSample = ui.textures[uiObj.albedoIdx].Sample(ui.sampler, input.uv);
    float alpha      = texSample.a;

    if (uiObj.isSDF != 0) {
        float distance  = texSample.a;
        float smoothing = clamp(fwidth(distance) * 0.5f, 0.0001f, 0.5f);
        alpha           = smoothstep(0.5f - smoothing, 0.5f + smoothing, distance);
    }

    float3 color = uiObj.useTextureColor != 0 ? texSample.rgb * input.color.rgb : input.color.rgb;
    return float4(color, input.color.a * alpha);
}
