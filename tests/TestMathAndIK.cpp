// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TestsFramework.hpp"
#include <Zahlen/Camera.hpp>
#include <Zahlen/IK.hpp>
#include <Zahlen/Math3D.hpp>
#include <cmath>
#include <expected>

enum class MathTestError : uint32_t {
    Success = 0,
    IKSolverFailed[[= ZHLN::Reflect::Description("TwoBoneIK solver produced invalid joint orientations or positions.")]],
    FrustumCullingFailed[[= ZHLN::Reflect::Description("Frustum sphere culling falsely classified an inside/outside object.")]],
    PackingLossyMismatch[[= ZHLN::Reflect::Description("Bit-packed normal/UV/color compression lost excessive precision.")]],
    MatrixProjectionFailed[[= ZHLN::Reflect::Description("Perspective matrix did not conform to Vulkan Y-down or NDC clip depth.")]],
};

struct MathAndIKTestSuite {
    struct Tests {
        // --- 1. Analytic 2-Bone IK Solver ---
        std::expected<void, ZHLN::Error> two_bone_ik_solver() {
            // Setup limb: Upper Arm (len 2.0) + Lower Arm (len 2.0) = Max reach 4.0
            ZHLN::IK::TwoBoneIKSolverInput input {
                .upperPosition  = JPH::Vec3(0.0f, 2.0f, 0.0f),
                .targetPosition = JPH::Vec3(0.0f, 2.0f, 2.8284f), // Reach ~2.828m (90-degree right angle)
                .poleVector     = JPH::Vec3(0.0f, 1.0f, 0.0f),    // Elbow points UP (+Y)
                .upperLength    = 2.0f,
                .lowerLength    = 2.0f
            };

            ZHLN::IK::TwoBoneIKSolverOutput output = ZHLN::IK::SolveTwoBoneIK(input);

            ZHLN::Test::ExpectTrue(output.valid);

            // Middle joint (elbow) should be elevated in +Y due to pole vector
            ZHLN::Test::ExpectTrue(output.midPosition.GetY() > input.upperPosition.GetY());

            // Length from upper -> mid must equal upperLength
            float upperDist = (output.midPosition - input.upperPosition).Length();
            ZHLN::Test::ExpectTrue(std::abs(upperDist - 2.0f) < 0.01f);

            // Length from mid -> target must equal lowerLength
            float lowerDist = (input.targetPosition - output.midPosition).Length();
            ZHLN::Test::ExpectTrue(std::abs(lowerDist - 2.0f) < 0.01f);

            // Test 1b: Target out-of-reach clamping (target distance 10.0m > 4.0m)
            input.targetPosition = JPH::Vec3(0.0f, 2.0f, 10.0f);
            output               = ZHLN::IK::SolveTwoBoneIK(input);
            ZHLN::Test::ExpectTrue(output.valid);

            return {};
        }

        // --- 2. Frustum Plane Extraction & SIMD SoA Culling ---
        std::expected<void, ZHLN::Error> frustum_culling_simd() {
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
        std::expected<void, ZHLN::Error> vertex_attribute_packing() {
            // 1. Pack 10-10-10-2 Normal
            ZHLN::Packed1010102 packedNorm = ZHLN::Math::PackNormal(0.0f, 1.0f, 0.0f, 1.0f);
            ZHLN::Test::ExpectTrue(packedNorm.data != 0);

            // 2. Pack 16-bit Half Float UVs
            ZHLN::PackedHalf2 packedUV = ZHLN::Math::PackUV(0.5f, 0.75f);
            uint16_t          halfU    = packedUV.data & 0xFFFF;
            uint16_t          halfV    = packedUV.data >> 16;
            ZHLN::Test::ExpectTrue(halfU != 0 && halfV != 0);

            // 3. RGBA8 Pack Color
            ZHLN::PackedRGBA8 packedCol = ZHLN::Math::PackColor(1.0f, 0.0f, 0.0f, 1.0f);
            ZHLN::Test::ExpectEq(packedCol.data & 0xFF, 255u); // Red = 255
            ZHLN::Test::ExpectEq((packedCol.data >> 8) & 0xFF, 0u);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<MathAndIKTestSuite>();
}
