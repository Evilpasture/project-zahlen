// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// GPU BRDF split-sum LUT. Same GGX / Schlick-Smith integration as the
// historical CPU path, writing packed RG8 into a device-address buffer.
import sampling;

public struct BRDFLUTPush {
    public uint64_t outAddr;
    public uint width;
    public uint height;
    public uint sampleCount;
};

[vk::push_constant]
BRDFLUTPush pc;

[shader("compute")]
[numthreads(16, 16, 1)]
void CSMain(uint3 tid: SV_DispatchThreadID) {
    if (tid.x >= pc.width || tid.y >= pc.height)
        return;

    float roughness = max(float(tid.y) / float(max(pc.height, 2u) - 1u), 0.001f);
    float NdotV     = max(float(tid.x) / float(max(pc.width, 2u) - 1u), 0.001f);

    float3 V = float3(sqrt(max(1.0f - NdotV * NdotV, 0.0f)), 0.0f, NdotV);
    float A  = 0.0f;
    float B  = 0.0f;

    uint samples = max(pc.sampleCount, 1u);
    for (uint i = 0; i < samples; ++i) {
        float2 xi   = Hammersley(i, samples);
        float3 H    = ImportanceSampleGGX(xi.x, xi.y, roughness);
        float3 L    = normalize(2.0f * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0f);
        float NdotH = max(H.z, 0.0f);
        float VdotH = max(dot(V, H), 0.0f);
        if (NdotL > 0.0f) {
            float k            = (roughness * roughness) / 2.0f;
            float Vis_SchlickV = NdotV * (1.0f - k) + k;
            float Vis_SchlickL = NdotL * (1.0f - k) + k;
            float G_Vis        = (NdotL * VdotH) / (Vis_SchlickV * Vis_SchlickL * max(NdotH, 0.001f));
            float Fc           = pow(max(1.0f - VdotH, 0.0f), 5.0f);
            A += (1.0f - Fc) * G_Vis;
            B += Fc * G_Vis;
        }
    }

    A /= float(samples);
    B /= float(samples);

    uint* pixels                 = (uint*) pc.outAddr;
    pixels[tid.y * pc.width + tid.x] = PackRG8(A, B);
}
