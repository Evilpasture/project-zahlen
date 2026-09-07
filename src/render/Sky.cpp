// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include <Utils.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <cstddef>
#include <numbers>

namespace ZHLN::PBR {

auto Hammersley(uint32_t i, uint32_t N) -> std::pair<float, float>;
auto ImportanceSampleGGX(float u1, float u2, float roughness) -> JPH::Vec3;

inline auto LerpColor(const JPH::Vec3& a, const JPH::Vec3& b, float t) -> JPH::Vec3 {
    return a + t * (b - a);
}

auto EvaluateSky(const JPH::Vec3& D) -> JPH::Vec3 {
    float dy = D.GetY();

    JPH::Vec3 zenith(0.12f, 0.38f, 0.85f);
    JPH::Vec3 horizon(0.55f, 0.82f, 1.00f);
    JPH::Vec3 ground(0.20f, 0.15f, 0.10f);

    JPH::Vec3 color {};
    if (dy >= 0.0f) {
        color = LerpColor(horizon, zenith, std::pow(dy, 1.2f));
    } else {
        color = LerpColor(horizon, ground, std::pow(-dy, 0.5f));
    }

    JPH::Vec3 sunDir   = JPH::Vec3(0.5f, 1.0f, 0.2f).Normalized();
    float     cosTheta = D.Dot(sunDir);
    if (cosTheta > 0.0f) {
        float glow = std::pow(cosTheta, 24.0f) * 0.7f;
        float disk = std::pow(cosTheta, 400.0f) * 2.5f;
        color += JPH::Vec3(1.0f, 1.0f, 1.0f) * (glow + disk);
    }

    return color;
}

auto GetCubeDirection(int face, float u, float v) -> JPH::Vec3 {
    float uc = u * 2.0f - 1.0f;
    float vc = v * 2.0f - 1.0f;

    switch (face) {
        case 0:
            return JPH::Vec3(1.0f, -vc, -uc).Normalized();
        case 1:
            return JPH::Vec3(-1.0f, -vc, uc).Normalized();
        case 2:
            return JPH::Vec3(uc, 1.0f, -vc).Normalized();
        case 3:
            return JPH::Vec3(uc, -1.0f, vc).Normalized();
        case 4:
            return JPH::Vec3(uc, -vc, 1.0f).Normalized();
        case 5:
            return JPH::Vec3(-uc, -vc, -1.0f).Normalized();
    }
    return JPH::Vec3::sAxisY();
}

auto PackColor(const JPH::Vec3& color) -> uint32_t {
    auto r = static_cast<uint8_t>(ZHLN::Clamp(color.GetX(), 0.0f, 1.0f) * 255.0f);
    auto g = static_cast<uint8_t>(ZHLN::Clamp(color.GetY(), 0.0f, 1.0f) * 255.0f);
    auto b = static_cast<uint8_t>(ZHLN::Clamp(color.GetZ(), 0.0f, 1.0f) * 255.0f);
    return 0xFF000000u | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

auto GenerateCosHemisphere(float u1, float u2) -> JPH::Vec3 {
    float phi      = 2.0f * std::numbers::pi_v<float> * u1;
    float cosTheta = std::sqrt(1.0f - u2);
    float sinTheta = std::sqrt(u2);
    return {std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta};
}

auto GenerateDiffuseSH() -> std::array<JPH::Vec4, 9> {
    std::array<JPH::Vec4, 9> sh {};
    for (int i = 0; i < 9; ++i) {
        sh[i] = JPH::Vec4::sZero();
    }

    const uint32_t SAMPLE_COUNT = 16384;
    const float    weight       = 4.0f * std::numbers::pi_v<float> / static_cast<float>(SAMPLE_COUNT);

    for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
        auto [u1, u2] = Hammersley(i, SAMPLE_COUNT);

        float z   = 1.0f - 2.0f * u1;
        float r   = std::sqrt(std::max(0.0f, 1.0f - z * z));
        float phi = 2.0f * std::numbers::pi_v<float> * u2;

        float     x = r * std::cos(phi);
        float     y = r * std::sin(phi);
        JPH::Vec3 dir(x, y, z);

        JPH::Vec3 L_in = EvaluateSky(dir);

        float Y[9];
        Y[0] = 0.282095f;
        Y[1] = -0.488603f * y;
        Y[2] = 0.488603f * z;
        Y[3] = -0.488603f * x;
        Y[4] = 1.092548f * x * y;
        Y[5] = -1.092548f * y * z;
        Y[6] = 0.315392f * (3.0f * z * z - 1.0f);
        Y[7] = -1.092548f * x * z;
        Y[8] = 0.546274f * (x * x - y * y);

        for (int c = 0; c < 9; ++c) {
            sh[c] += JPH::Vec4(L_in * Y[c] * weight, 0.0f);
        }
    }

    const float A0 = 1.0f;
    const float A1 = 2.0f / 3.0f;
    const float A2 = 0.25f;

    sh[0] *= A0;
    sh[1] *= A1;
    sh[2] *= A1;
    sh[3] *= A1;
    sh[4] *= A2;
    sh[5] *= A2;
    sh[6] *= A2;
    sh[7] *= A2;
    sh[8] *= A2;

    return sh;
}

auto GenerateSpecularMip(uint32_t size, float roughness) -> std::vector<std::vector<uint32_t>> {
    std::vector<std::vector<uint32_t>> cubemap(6, std::vector<uint32_t>(static_cast<size_t>(size * size)));

    for (int face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < size; ++y) {
            float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(size);
            for (uint32_t x = 0; x < size; ++x) {
                float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(size);

                JPH::Vec3 R = GetCubeDirection(face, u, v);

                if (roughness == 0.0f) {
                    cubemap[face][y * size + x] = PackColor(EvaluateSky(R));
                    continue;
                }

                JPH::Vec3 N = R;
                JPH::Vec3 V = R;

                JPH::Vec3 prefilteredColor = JPH::Vec3::sZero();
                float     totalWeight      = 0.0f;

                const uint32_t SAMPLE_COUNT = 32;
                for (uint32_t i = 0; i < SAMPLE_COUNT; ++i) {
                    auto [u1, u2] = Hammersley(i, SAMPLE_COUNT);
                    JPH::Vec3 H   = ImportanceSampleGGX(u1, u2, roughness);

                    JPH::Vec3 up        = std::abs(N.GetY()) < 0.999f ? JPH::Vec3::sAxisY() : JPH::Vec3::sAxisZ();
                    JPH::Vec3 tangent   = up.Cross(N).Normalized();
                    JPH::Vec3 bitangent = N.Cross(tangent);
                    JPH::Vec3 worldH    = tangent * H.GetX() + bitangent * H.GetY() + N * H.GetZ();

                    JPH::Vec3 L     = (2.0f * V.Dot(worldH) * worldH - V).Normalized();
                    float     NdotL = std::max(L.Dot(N), 0.0f);

                    if (NdotL > 0.0f) {
                        prefilteredColor += EvaluateSky(L) * NdotL;
                        totalWeight += NdotL;
                    }
                }

                if (totalWeight > 0.0f) {
                    prefilteredColor /= totalWeight;
                }
                cubemap[face][y * size + x] = PackColor(prefilteredColor);
            }
        }
    }
    return cubemap;
}

} // namespace ZHLN::PBR
