// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Debug lines. Deliberately not basic.slang: a line is not a scene mesh, so it
// has no material, no bindless textures and no entry in the instance table.
// The vertex shader reads one interleaved buffer through its device address
// and transforms by a view-projection that arrives in the push block, which
// leaves this shader with no descriptor set at all -- no heap mapping, no
// binding table, nothing for the host to reflect.

import gpu_buffer;

struct LineVertex {
    float3 position;
    uint   color; // packed RGBA8, host side Math::PackColor
};

struct LinePushConstants {
    float4x4 viewProj;
    uint64_t vertexBufferAddress;
};

[vk::push_constant]
LinePushConstants pc;

struct VSOutput {
    float4 pos : SV_Position;
    float4 color : COLOR;
};

float4 UnpackRGBA8(uint packed) {
    return float4(
        float(packed & 0xFF) / 255.0f,
        float((packed >> 8) & 0xFF) / 255.0f,
        float((packed >> 16) & 0xFF) / 255.0f,
        float((packed >> 24) & 0xFF) / 255.0f
    );
}

[shader("vertex")]
VSOutput VSMain(uint vertexId : SV_VertexID) {
    VSOutput output;
    GPUBuffer<LineVertex> vertices = GPUBuffer<LineVertex>(pc.vertexBufferAddress);
    LineVertex v = vertices.load(vertexId);

    output.pos   = mul(pc.viewProj, float4(v.position, 1.0f));
    output.color = UnpackRGBA8(v.color);
    return output;
}

[shader("fragment")]
float4 PSMain(VSOutput input) : SV_Target0 {
    return input.color;
}
