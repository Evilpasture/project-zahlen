// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Core math tests. The two-bone IK solver test moved with its code to
// tests/extras/TestTwoBoneIK.cpp when IK left core for extensions/Animation.
#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Math3D.hpp>
#include <cstdint>

namespace ZHLN::Test {
namespace {

struct ZHLN_ANNOTATION(SuiteFixture {}) DefaultCameraFrustum {
    ZHLN::Frustum frustum {};

    DefaultCameraFrustum() {
        ZHLN::Camera cam;
        cam.position = JPH::Vec3(0.0f, 0.0f, 10.0f);
        cam.yaw      = -90.0f; // Look towards -Z
        cam.pitch    = 0.0f;
        cam.fov      = 60.0f;
        cam.nearZ    = 0.1f;
        cam.farZ     = 100.0f;

        const JPH::Mat44 vp = cam.GetProjectionMatrix(16.0f / 9.0f) * cam.GetViewMatrix();
        frustum.Update(vp);
    }
};

// --- 1. Freestanding Scalar Math ---
void test_scalar_math() {
    static_assert(ZHLN::Math::Floor(-1.25) == -2.0);
    static_assert(ZHLN::Math::Fract(-1.25) == 0.75);
    static_assert(ZHLN::Math::Min({7, 3, 5}) == 3);
    static_assert(ZHLN::Math::Max(3, 7) == 7);
    static_assert(ZHLN::Math::Clamp(-2, 0, 1) == 0);
    static_assert(ZHLN::Math::Saturate(2.0f) == 1.0f);
    static_assert(
        ZHLN::Math::PackColor(static_cast<uint8_t>(0x12), static_cast<uint8_t>(0x34), static_cast<uint8_t>(0x56)) == 0xFF563412U
    );

    constexpr float bakedPower = ZHLN::Math::Power(2.0f, -3);
    constexpr float bakedRoot  = ZHLN::Math::Sqrt(9.0f);
    static_assert(bakedPower == 0.125f);
    static_assert(bakedRoot > 2.99f && bakedRoot < 3.01f);

    Expect(ZHLN::Math::Smoothstep(0.0f, 1.0f, 0.5f)) == 0.5f;
    Expect(ZHLN::Math::Abs(-7)) == 7;
    Expect(ZHLN::Math::Lerp(2.0f, 6.0f, 0.25f)) == 3.0f;

    const float noise = ZHLN::Math::FBM(0.25f, 0.75f, 4);
    const float cell  = ZHLN::Math::Worley(0.25f, 0.75f);
    ExpectInRange(noise, 0.0f, 1.0f);
    ExpectInRange(cell, 0.0f, 1.0f);
}

// --- 2. Frustum Plane Extraction & SIMD SoA Culling ---
ZHLN_ANNOTATION(Cases {
    NamedCase {"inside_front", 0.0f, 0.0f, 0.0f, true},
    NamedCase {"behind_camera", 0.0f, 0.0f, 25.0f, false},
    NamedCase {"beyond_far_plane", 0.0f, 0.0f, -150.0f, false},
    NamedCase {"outside_left", -100.0f, 0.0f, 0.0f, false},
})
void test_frustum_culling_simd(
    const DefaultCameraFrustum& camFrustum,
    float                       x,
    float                       y,
    float                       z,
    bool                        expectedVisible
) {
    Expect(camFrustum.frustum.IsSphereVisible(JPH::Vec3(x, y, z), 1.0f)) == expectedVisible;
}

// --- 3. Packed Vertex Attributes & Half-Float Encoding ---
void test_vertex_attribute_packing() {
    // 1. Pack 10-10-10-2 Normal
    ZHLN::Packed1010102 packedNorm = ZHLN::Math::PackNormal(0.0f, 1.0f, 0.0f, 1.0f);
    Expect(packedNorm.data) != 0u;

    // 2. Pack 16-bit Half Float UVs
    ZHLN::PackedHalf2 packedUV = ZHLN::Math::PackUV(0.5f, 0.75f);
    uint16_t          halfU    = packedUV.data & 0xFFFF;
    uint16_t          halfV    = packedUV.data >> 16;
    Expect(halfU) != 0;
    Expect(halfV) != 0;

    // 3. RGBA8 Pack Color
    ZHLN::PackedRGBA8 packedCol = ZHLN::Math::PackColor(1.0f, 0.0f, 0.0f, 1.0f);
    Expect(packedCol.data & 0xFF) == 255u;
    Expect((packedCol.data >> 8) & 0xFF) == 0u;
}

} // namespace
} // namespace ZHLN::Test
