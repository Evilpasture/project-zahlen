// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Two-bone IK left core with the rest of the analytic-IK toolkit
// (extras/Animation, ZHLN::IK); its solver test moved with it. The scalar
// math, frustum and packing tests stayed in tests/core/TestMath.cpp.
#include "TestsFramework.hpp"
#include <Animation/IK.hpp>
#include <cmath>
#include <expected>

enum class TwoBoneIKTestError : uint32_t {
    IKSolverFailed ZHLN_ANNOTATION(ZHLN::Description<"TwoBoneIK solver produced invalid joint orientations or positions.">{}) = 1,
};

struct TwoBoneIKTestSuite {
    struct Tests {
        // --- Analytic 2-Bone IK Solver ---
        std::expected<void, ZHLN::ErrorCode> two_bone_ik_solver() {
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
            ZHLN::Test::ExpectGt(output.midPosition.GetY(), input.upperPosition.GetY());

            // Length from upper -> mid must equal upperLength
            float upperDist = (output.midPosition - input.upperPosition).Length();
            ZHLN::Test::ExpectLt(std::abs(upperDist - 2.0f), 0.01f);

            // Length from mid -> constrained end must equal lowerLength
            float lowerDist = (output.endPosition - output.midPosition).Length();
            ZHLN::Test::ExpectLt(std::abs(lowerDist - 2.0f), 0.01f);

            // Test 1b: Target out-of-reach clamping (target distance 10.0m > 4.0m)
            input.targetPosition = JPH::Vec3(0.0f, 2.0f, 10.0f);
            input.maxExtension   = 0.98f;
            output               = ZHLN::IK::SolveTwoBoneIK(input);
            ZHLN::Test::ExpectTrue(output.valid);
            ZHLN::Test::ExpectTrue(output.reachClamped);
            ZHLN::Test::ExpectLt(std::abs(output.solvedDistance - 3.92f), 0.001f);
            ZHLN::Test::ExpectLt(std::abs((output.midPosition - input.upperPosition).Length() - input.upperLength), 0.001f);
            ZHLN::Test::ExpectLt(std::abs((output.endPosition - output.midPosition).Length() - input.lowerLength), 0.001f);

            return {};
        }
    };
};

int main() {
    return ZHLN::Test::Runner::Run<TwoBoneIKTestSuite>();
}
