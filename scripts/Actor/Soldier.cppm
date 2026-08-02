// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

module;
#include <Jolt/Jolt.h>
#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Quat.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Math/Vec4.h>
#include <Zahlen/Math3D.hpp>
#include <Zahlen/Render.hpp>
#include <Zahlen/Types.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

export module ZHLN.Soldier;

import ZHLN.Rig;
import ZHLN.MathUtils;

namespace ZHLN::Soldier {

struct WSpec {
    Rig::Joint self   = Rig::Joint::Hips;
    int32_t    parent = -1;
    int32_t    child  = -1;
    float      k      = 0.34f;
};

// Smoothstep blending [0, 1]
inline float Smooth01(float x) noexcept {
    float t = MathUtils::Clamp(x, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

class ProceduralSoldierBuilder {
  public:
    std::vector<VertexPosition>   positions;
    std::vector<VertexAttributes> attributes;
    std::vector<VertexSkin>       skin;
    std::vector<uint32_t>         indices;

    [[nodiscard]] uint32_t VertCount() const noexcept {
        return static_cast<uint32_t>(positions.size());
    }

    void PushVert(JPH::Vec3Arg p, JPH::Vec3Arg n, JPH::Vec4Arg c, const std::array<uint16_t, 4>& j, const std::array<float, 4>& w) {
        positions.push_back({{p.GetX(), p.GetY(), p.GetZ()}});

        Packed1010102 norm = Math::PackNormal(n.GetX(), n.GetY(), n.GetZ());
        Packed1010102 tang = Math::PackNormal(1.0f, 0.0f, 0.0f);
        PackedRGBA8   col  = Math::PackColor(c.GetX(), c.GetY(), c.GetZ(), c.GetW());
        attributes.push_back({.normal = norm, .tangent = tang, .uv = Math::PackUV(0.0f, 0.0f), .color = col});

        float sum = w[0] + w[1] + w[2] + w[3];
        if (sum <= 0.0001f)
            sum = 1.0f;

        uint8_t w0 = static_cast<uint8_t>((w[0] / sum) * 255.0f);
        uint8_t w1 = static_cast<uint8_t>((w[1] / sum) * 255.0f);
        uint8_t w2 = static_cast<uint8_t>((w[2] / sum) * 255.0f);
        uint8_t w3 = static_cast<uint8_t>((w[3] / sum) * 255.0f);

        skin.push_back(
            {.joints = {j[0], j[1], j[2], j[3]}, .weights = PackedRGBA8 {(uint32_t(w3) << 24) | (uint32_t(w2) << 16) | (uint32_t(w1) << 8) | uint32_t(w0)}}
        );
    }

    std::pair<std::array<uint16_t, 4>, std::array<float, 4>> EvaluateWeights(float t, const WSpec& s) const noexcept {
        float wp    = (s.parent >= 0) ? 0.5f * (1.0f - Smooth01(t / s.k)) : 0.0f;
        float wc    = (s.child >= 0) ? 0.5f * Smooth01((t - (1.0f - s.k)) / s.k) : 0.0f;
        float wSelf = std::max(0.001f, 1.0f - wp - wc);

        std::array<uint16_t, 4> j = {static_cast<uint16_t>(s.self), 0, 0, 0};
        std::array<float, 4>    w = {wSelf, 0.0f, 0.0f, 0.0f};

        size_t count = 1;
        if (wp > 0.001f && count < 4) {
            j[count] = static_cast<uint16_t>(s.parent);
            w[count] = wp;
            count++;
        }
        if (wc > 0.001f && count < 4) {
            j[count] = static_cast<uint16_t>(s.child);
            w[count] = wc;
            count++;
        }
        return {j, w};
    }

    void Blob(
        JPH::Vec3Arg                   center,
        float                          r,
        JPH::Vec4Arg                   color,
        const std::array<uint16_t, 4>& j,
        const std::array<float, 4>&    w,
        float                          sx    = 1.0f,
        float                          sy    = 1.0f,
        float                          sz    = 1.0f,
        int                            segs  = 9,
        int                            rings = 6
    ) {
        uint32_t base = VertCount();

        for (int i = 0; i <= rings; ++i) {
            float phi = (static_cast<float>(i) / static_cast<float>(rings)) * JPH::JPH_PI;
            for (int jIdx = 0; jIdx < segs; ++jIdx) {
                float     theta = (static_cast<float>(jIdx) / static_cast<float>(segs)) * JPH::JPH_PI * 2.0f;
                JPH::Vec3 norm(std::sin(phi) * std::cos(theta), std::cos(phi), std::sin(phi) * std::sin(theta));
                JPH::Vec3 pos = center + JPH::Vec3(norm.GetX() * r * sx, norm.GetY() * r * sy, norm.GetZ() * r * sz);

                PushVert(pos, norm.Normalized(), color, j, w);
            }
        }

        for (int i = 0; i < rings; ++i) {
            for (int jIdx = 0; jIdx < segs; ++jIdx) {
                uint32_t j2 = (jIdx + 1) % segs;
                uint32_t a0 = base + i * segs + jIdx;
                uint32_t a1 = base + i * segs + j2;
                uint32_t b0 = base + (i + 1) * segs + jIdx;
                uint32_t b1 = base + (i + 1) * segs + j2;

                indices.push_back(a0);
                indices.push_back(a1);
                indices.push_back(b0);
                indices.push_back(a1);
                indices.push_back(b1);
                indices.push_back(b0);
            }
        }
    }

    void Tube(
        JPH::Vec3Arg a,
        JPH::Vec3Arg b,
        float        r0,
        float        r1,
        JPH::Vec4Arg color,
        WSpec        w,
        int          sides = 9,
        int          rings = 4,
        float        sx    = 1.0f,
        float        sz    = 1.0f,
        bool         cap   = true
    ) {
        JPH::Vec3 axis = b - a;
        float     len  = axis.Length();
        if (len < 0.0001f)
            len = 0.0001f;
        axis /= len;

        JPH::Vec3 up = (std::abs(axis.GetY()) > 0.9f) ? JPH::Vec3(0, 0, 1) : JPH::Vec3(0, 1, 0);
        JPH::Vec3 ex = up.Cross(axis).Normalized();
        JPH::Vec3 ez = axis.Cross(ex).Normalized();

        uint32_t base = VertCount();

        for (int i = 0; i < rings; ++i) {
            float     t       = static_cast<float>(i) / static_cast<float>(rings - 1);
            JPH::Vec3 center  = a + (b - a) * t;
            float     r       = r0 + (r1 - r0) * t;
            auto [j, weights] = EvaluateWeights(t, w);

            for (int jIdx = 0; jIdx < sides; ++jIdx) {
                float ang = (static_cast<float>(jIdx) / static_cast<float>(sides)) * JPH::JPH_PI * 2.0f;
                float dx  = std::cos(ang) * sx;
                float dz  = std::sin(ang) * sz;

                JPH::Vec3 norm = (ex * (dx / (sx * sx)) + ez * (dz / (sz * sz))).Normalized();
                JPH::Vec3 pos  = center + ex * (dx * r) + ez * (dz * r);

                PushVert(pos, norm, color, j, weights);
            }
        }

        for (int i = 0; i < rings - 1; ++i) {
            for (int jIdx = 0; jIdx < sides; ++jIdx) {
                uint32_t j2 = (jIdx + 1) % sides;
                uint32_t a0 = base + i * sides + jIdx;
                uint32_t a1 = base + i * sides + j2;
                uint32_t b0 = base + (i + 1) * sides + jIdx;
                uint32_t b1 = base + (i + 1) * sides + j2;

                indices.push_back(a0);
                indices.push_back(a1);
                indices.push_back(b0);
                indices.push_back(a1);
                indices.push_back(b1);
                indices.push_back(b0);
            }
        }

        if (cap) {
            std::array<uint16_t, 4> jSelf = {static_cast<uint16_t>(w.self), 0, 0, 0};
            std::array<float, 4>    wSelf = {1.0f, 0.0f, 0.0f, 0.0f};

            std::array<uint16_t, 4> jEnd = {static_cast<uint16_t>(w.child >= 0 ? w.child : static_cast<int32_t>(w.self)), 0, 0, 0};
            std::array<float, 4>    wEnd = {1.0f, 0.0f, 0.0f, 0.0f};

            Blob(a, r0, color, jSelf, wSelf, sx, 1.0f, sz, 7, 4);
            Blob(b, r1, color, jEnd, wEnd, sx, 1.0f, sz, 7, 4);
        }
    }

    void Box(JPH::Vec3Arg center, JPH::Vec3Arg size, JPH::Vec4Arg color, const std::array<uint16_t, 4>& j, const std::array<float, 4>& w, float rotX = 0.0f) {
        JPH::Quat rot = JPH::Quat::sRotation(JPH::Vec3::sAxisX(), rotX);
        JPH::Vec3 hs  = size * 0.5f;

        struct Face {
            JPH::Vec3 n, u, v;
        };
        std::array<Face, 6> faces = {{
            {JPH::Vec3(0, 0, 1), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 0, -1), JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(-1, 0, 0), JPH::Vec3(0, 0, 1), JPH::Vec3(0, 1, 0)},
            {JPH::Vec3(0, 1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, -1)},
            {JPH::Vec3(0, -1, 0), JPH::Vec3(1, 0, 0), JPH::Vec3(0, 0, 1)},
        }};

        for (const auto& f: faces) {
            uint32_t  base = VertCount();
            JPH::Vec3 norm = rot * f.n;

            std::array<std::pair<float, float>, 4> uvs = {{{-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f}, {-1.0f, 1.0f}}};

            for (const auto& [su, sv]: uvs) {
                JPH::Vec3 offset = f.n * hs + (f.u * hs) * su + (f.v * hs) * sv;
                JPH::Vec3 pos    = center + rot * offset;
                PushVert(pos, norm, color, j, w);
            }

            indices.push_back(base);
            indices.push_back(base + 1);
            indices.push_back(base + 2);
            indices.push_back(base);
            indices.push_back(base + 2);
            indices.push_back(base + 3);
        }
    }
};

export Mesh GenerateSkinnedSoldierMesh(RenderContext& rc) {
    ProceduralSoldierBuilder builder;

    // Palette Definition
    JPH::Vec4 C_FATIGUE(0.29f, 0.33f, 0.25f, 1.0f);
    JPH::Vec4 C_FATIGUE_DARK(0.24f, 0.27f, 0.21f, 1.0f);
    JPH::Vec4 C_VEST(0.15f, 0.16f, 0.17f, 1.0f);
    JPH::Vec4 C_POUCH(0.19f, 0.21f, 0.17f, 1.0f);
    JPH::Vec4 C_HELMET(0.18f, 0.20f, 0.17f, 1.0f);
    JPH::Vec4 C_SKIN(0.66f, 0.47f, 0.31f, 1.0f);
    JPH::Vec4 C_GLOVE(0.11f, 0.12f, 0.13f, 1.0f);
    JPH::Vec4 C_BOOT(0.08f, 0.08f, 0.09f, 1.0f);
    JPH::Vec4 C_VISOR(0.08f, 0.09f, 0.11f, 1.0f);
    JPH::Vec4 C_PAD(0.13f, 0.15f, 0.12f, 1.0f);

    // Compute World T-Pose Bind Positions
    std::array<JPH::Vec3, Rig::JointCount> W;
    for (uint32_t i = 0; i < Rig::JointCount; ++i) {
        Rig::Joint j      = static_cast<Rig::Joint>(i);
        JPH::Vec3  local  = Rig::GetBindPosition(j);
        int32_t    parent = Rig::GetParentIndex(j);
        W[i]              = (parent >= 0) ? W[parent] + local : local;
    }

    auto V = [&W](Rig::Joint j) { return W[static_cast<size_t>(j)]; };

    // 1. TORSO & CLOTHING
    builder.Tube(
        V(Rig::Joint::Hips), V(Rig::Joint::Spine), 0.155f, 0.160f, C_FATIGUE, {.self = Rig::Joint::Hips, .child = static_cast<int32_t>(Rig::Joint::Spine)}, 9,
        3, 1.25f, 0.86f
    );
    builder.Tube(
        V(Rig::Joint::Spine), V(Rig::Joint::Chest), 0.160f, 0.185f, C_FATIGUE,
        {.self = Rig::Joint::Spine, .parent = static_cast<int32_t>(Rig::Joint::Hips), .child = static_cast<int32_t>(Rig::Joint::Chest)}, 9, 3, 1.28f, 0.88f
    );
    builder.Tube(
        V(Rig::Joint::Chest), V(Rig::Joint::Neck), 0.185f, 0.115f, C_FATIGUE, {.self = Rig::Joint::Chest, .parent = static_cast<int32_t>(Rig::Joint::Spine)}, 9,
        3, 1.26f, 0.90f
    );

    // Plate Carrier Vest
    builder.Tube(
        JPH::Vec3(0.0f, 1.09f, 0.0f), JPH::Vec3(0.0f, 1.45f, 0.0f), 0.185f, 0.200f, C_VEST,
        {.self = Rig::Joint::Spine, .parent = static_cast<int32_t>(Rig::Joint::Hips), .child = static_cast<int32_t>(Rig::Joint::Chest), .k = 0.3f}, 9, 4, 1.28f,
        0.95f
    );

    // Tactical Pouches
    std::array<uint16_t, 4> jSpine = {static_cast<uint16_t>(Rig::Joint::Spine), 0, 0, 0};
    std::array<uint16_t, 4> jChest = {static_cast<uint16_t>(Rig::Joint::Chest), 0, 0, 0};
    std::array<float, 4>    wFull  = {1.0f, 0.0f, 0.0f, 0.0f};

    builder.Box(JPH::Vec3(0.08f, 1.16f, 0.19f), JPH::Vec3(0.11f, 0.11f, 0.07f), C_POUCH, jSpine, wFull);
    builder.Box(JPH::Vec3(-0.05f, 1.16f, 0.20f), JPH::Vec3(0.11f, 0.11f, 0.07f), C_POUCH, jSpine, wFull);
    builder.Box(JPH::Vec3(0.00f, 1.30f, -0.20f), JPH::Vec3(0.26f, 0.22f, 0.09f), C_POUCH, jChest, wFull);

    // Belt
    builder.Tube(JPH::Vec3(0.0f, 0.99f, 0.0f), JPH::Vec3(0.0f, 1.05f, 0.0f), 0.168f, 0.168f, C_BOOT, {.self = Rig::Joint::Hips}, 9, 2, 1.24f, 0.90f, false);

    // 2. HEAD, HELMET & VISOR
    std::array<uint16_t, 4> jHead = {static_cast<uint16_t>(Rig::Joint::Head), 0, 0, 0};
    builder.Tube(
        V(Rig::Joint::Neck), V(Rig::Joint::Head), 0.062f, 0.070f, C_SKIN, {.self = Rig::Joint::Neck, .child = static_cast<int32_t>(Rig::Joint::Head)}, 7, 2
    );
    builder.Blob(JPH::Vec3(0.0f, 1.68f, 0.01f), 0.108f, C_SKIN, jHead, wFull, 1.0f, 1.15f, 1.08f);

    // Helmet & Visor
    builder.Blob(JPH::Vec3(0.0f, 1.70f, -0.005f), 0.132f, C_HELMET, jHead, wFull, 1.0f, 1.00f, 1.06f, 9, 5);
    builder.Box(JPH::Vec3(0.0f, 1.665f, 0.10f), JPH::Vec3(0.19f, 0.055f, 0.11f), C_VISOR, jHead, wFull);
    builder.Box(JPH::Vec3(0.0f, 1.600f, 0.085f), JPH::Vec3(0.13f, 0.080f, 0.09f), C_PAD, jHead, wFull);

    // 3. ARMS (LEFT & RIGHT)
    struct ArmPair {
        Rig::Joint clav, arm, fore, hand;
    };
    std::array<ArmPair, 2> arms = {
        {{Rig::Joint::ClavicleL, Rig::Joint::UpperArmL, Rig::Joint::ForearmL, Rig::Joint::HandL},
         {Rig::Joint::ClavicleR, Rig::Joint::UpperArmR, Rig::Joint::ForearmR, Rig::Joint::HandR}}
    };

    for (const auto& a: arms) {
        builder.Tube(V(a.clav), V(a.arm), 0.085f, 0.075f, C_VEST, {.self = a.clav, .child = static_cast<int32_t>(a.arm)}, 8, 2, 1.0f, 1.0f, false);

        std::array<uint16_t, 4> jArm  = {static_cast<uint16_t>(a.arm), 0, 0, 0};
        std::array<uint16_t, 4> jHand = {static_cast<uint16_t>(a.hand), 0, 0, 0};

        builder.Blob(V(a.arm) + JPH::Vec3(0.0f, 0.02f, 0.0f), 0.085f, C_PAD, jArm, wFull, 1.0f, 1.0f, 1.0f, 8, 5);
        builder.Tube(
            V(a.arm), V(a.fore), 0.068f, 0.055f, C_FATIGUE, {.self = a.arm, .parent = static_cast<int32_t>(a.clav), .child = static_cast<int32_t>(a.fore)}, 8, 4
        );
        builder.Tube(
            V(a.fore), V(a.hand), 0.056f, 0.045f, C_FATIGUE_DARK,
            {.self = a.fore, .parent = static_cast<int32_t>(a.arm), .child = static_cast<int32_t>(a.hand)}, 8, 4
        );
        builder.Blob(V(a.hand) + JPH::Vec3(0.0f, -0.035f, 0.012f), 0.055f, C_GLOVE, jHand, wFull, 1.0f, 1.0f, 1.15f, 7, 5);
    }

    // 4. LEGS (LEFT & RIGHT)
    struct LegPair {
        Rig::Joint thigh, shin, foot, toe;
    };
    std::array<LegPair, 2> legs = {
        {{Rig::Joint::ThighL, Rig::Joint::ShinL, Rig::Joint::FootL, Rig::Joint::ToeL},
         {Rig::Joint::ThighR, Rig::Joint::ShinR, Rig::Joint::FootR, Rig::Joint::ToeR}}
    };

    for (const auto& l: legs) {
        builder.Tube(
            V(l.thigh), V(l.shin), 0.105f, 0.078f, C_FATIGUE,
            {.self = l.thigh, .parent = static_cast<int32_t>(Rig::Joint::Hips), .child = static_cast<int32_t>(l.shin)}, 9, 4
        );
        builder.Tube(
            V(l.shin), V(l.foot), 0.079f, 0.058f, C_FATIGUE, {.self = l.shin, .parent = static_cast<int32_t>(l.thigh), .child = static_cast<int32_t>(l.foot)},
            9, 4
        );

        std::array<uint16_t, 4> jShin     = {static_cast<uint16_t>(l.shin), 0, 0, 0};
        std::array<uint16_t, 4> jFoot     = {static_cast<uint16_t>(l.foot), 0, 0, 0};
        std::array<uint16_t, 4> jToeBlend = {static_cast<uint16_t>(l.toe), static_cast<uint16_t>(l.foot), 0, 0};
        std::array<float, 4>    wToeBlend = {0.7f, 0.3f, 0.0f, 0.0f};

        // Kneepad
        builder.Blob(V(l.shin) + JPH::Vec3(0.0f, 0.02f, 0.045f), 0.075f, C_PAD, jShin, wFull, 1.0f, 1.0f, 0.8f, 8, 5);

        // Boots
        builder.Box(JPH::Vec3(V(l.foot).GetX(), 0.05f, 0.03f), JPH::Vec3(0.11f, 0.10f, 0.16f), C_BOOT, jFoot, wFull);
        builder.Box(JPH::Vec3(V(l.toe).GetX(), 0.035f, 0.13f), JPH::Vec3(0.105f, 0.07f, 0.11f), C_BOOT, jToeBlend, wToeBlend);
    }

    // Upload to GPU Memory
    Mesh mesh;
    mesh.posBuffer   = rc.CreateVertexBuffer(builder.positions.data(), builder.positions.size() * sizeof(VertexPosition));
    mesh.attrBuffer  = rc.CreateVertexBuffer(builder.attributes.data(), builder.attributes.size() * sizeof(VertexAttributes));
    mesh.skinBuffer  = rc.CreateVertexBuffer(builder.skin.data(), builder.skin.size() * sizeof(VertexSkin));
    mesh.indexBuffer = rc.CreateIndexBuffer(builder.indices.data(), builder.indices.size() * sizeof(uint32_t));
    mesh.vertexCount = static_cast<uint32_t>(builder.positions.size());
    mesh.indexCount  = static_cast<uint32_t>(builder.indices.size());

    return mesh;
}

export AssetID GetOrGenerateSoldierMeshAsset(RenderContext& rc) {
    AssetID assetId = HashAssetID("procedural_skinned_soldier_mesh");
    if (!rc.GetGPUMesh(assetId).has_value()) {
        Mesh mesh = GenerateSkinnedSoldierMesh(rc);
        rc.RegisterGPUMesh(assetId, mesh);
    }
    return assetId;
}

} // namespace ZHLN::Soldier
