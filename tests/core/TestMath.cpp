// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Core math suite. The two-bone IK solver test moved with its code to
// tests/extras/TestTwoBoneIK.cpp when IK left core for extras/Animation.
#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <expected>

enum class MathTestError : uint32_t {
    FrustumCullingFailed ZHLN_ANNOTATION(ZHLN::Description<"Frustum sphere culling falsely classified an inside/outside object.">{}),
    PackingLossyMismatch ZHLN_ANNOTATION(ZHLN::Description<"Bit-packed normal/UV/color compression lost excessive precision.">{}),
    MatrixProjectionFailed ZHLN_ANNOTATION(ZHLN::Description<"Perspective matrix did not conform to Vulkan Y-down or NDC clip depth.">{}),
};

struct MathTestSuite {
    struct Tests {
        // --- 0. Freestanding Scalar Math ---
        std::expected<void, ZHLN::ErrorCode> scalar_math() {
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

            ZHLN::Test::ExpectEq(ZHLN::Math::Smoothstep(0.0f, 1.0f, 0.5f), 0.5f);
            ZHLN::Test::ExpectEq(ZHLN::Math::Abs(-7), 7);
            ZHLN::Test::ExpectEq(ZHLN::Math::Lerp(2.0f, 6.0f, 0.25f), 3.0f);

            const float noise = ZHLN::Math::FBM(0.25f, 0.75f, 4);
            const float cell  = ZHLN::Math::Worley(0.25f, 0.75f);
            ZHLN::Test::ExpectInRange(noise, 0.0f, 1.0f);
            ZHLN::Test::ExpectInRange(cell, 0.0f, 1.0f);
            return {};
        }

        // --- 2. Frustum Plane Extraction & SIMD SoA Culling ---
        std::expected<void, ZHLN::ErrorCode> frustum_culling_simd() {
            ZHLN::Camera cam;
            cam.position = JPH::Vec3(0.0f, 0.0f, 10.0f);
            cam.yaw      = -90.0f; // Look towards -Z
            cam.pitch    = 0.0f;
            cam.fov      = 60.0f;
            cam.nearZ    = 0.1f;
            cam.farZ     = 100.0f;

            JPH::Mat44 vp = cam.GetProjectionMatrix(16.0f / 9.0f) * cam.GetViewMatrix();
            cam.frustum.Update(vp);

            // 1. Point directly in front of camera (Inside)
            ZHLN::Test::ExpectTrue(cam.frustum.IsSphereVisible(JPH::Vec3(0.0f, 0.0f, 0.0f), 1.0f));

            // 2. Point far behind the camera (Outside)
            ZHLN::Test::ExpectFalse(cam.frustum.IsSphereVisible(JPH::Vec3(0.0f, 0.0f, 25.0f), 1.0f));

            // 3. Point far past the Far plane (Outside)
            ZHLN::Test::ExpectFalse(cam.frustum.IsSphereVisible(JPH::Vec3(0.0f, 0.0f, -150.0f), 1.0f));

            // 4. Point far to the left (Outside)
            ZHLN::Test::ExpectFalse(cam.frustum.IsSphereVisible(JPH::Vec3(-100.0f, 0.0f, 0.0f), 1.0f));

            return {};
        }

        // --- 3. Packed Vertex Attributes & Half-Float Encoding ---
        std::expected<void, ZHLN::ErrorCode> vertex_attribute_packing() {
            // 1. Pack 10-10-10-2 Normal
            ZHLN::Packed1010102 packedNorm = ZHLN::Math::PackNormal(0.0f, 1.0f, 0.0f, 1.0f);
            ZHLN::Test::ExpectTrue(packedNorm.data != 0);

            // 2. Pack 16-bit Half Float UVs
            ZHLN::PackedHalf2 packedUV = ZHLN::Math::PackUV(0.5f, 0.75f);
            uint16_t          halfU    = packedUV.data & 0xFFFF;
            uint16_t          halfV    = packedUV.data >> 16;
            ZHLN::Test::ExpectNe(halfU, 0) && ZHLN::Test::ExpectNe(halfV, 0);

            // 3. RGBA8 Pack Color
            ZHLN::PackedRGBA8 packedCol = ZHLN::Math::PackColor(1.0f, 0.0f, 0.0f, 1.0f);
            ZHLN::Test::ExpectEq(packedCol.data & 0xFF, 255u); // Red = 255
            ZHLN::Test::ExpectEq((packedCol.data >> 8) & 0xFF, 0u);

            return {};
        }
    };
};

// Exported for the core group binary (RunCoreTests.cpp), which
// aggregates every suite in this directory through Runner::RunDeferred.
auto RunMathSuite() -> ZHLN::Test::TestStats {
    return ZHLN::Test::RunSuite<MathTestSuite>();
}

