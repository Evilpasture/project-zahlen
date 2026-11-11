// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

// Core math tests. The two-bone IK solver test moved with its code to
// tests/extras/TestTwoBoneIK.cpp when IK left core for extensions/Animation.
#include "TestsFramework.hpp"
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Camera.hpp>
#include <Zahlen/Math/Transform.hpp>
#include <Zahlen/Math/Quat.hpp>
#include <Zahlen/Math/Scalar.hpp>
#include <Zahlen/ECS/Components.hpp>
#include <Zahlen/Core/Math.hpp>
#include <Zahlen/Math/Math3D.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <cmath>
#include <cstdint>

namespace ZHLN::Test {
namespace {

struct ZHLN_ANNOTATION(SuiteFixture {}) DefaultCameraFrustum {
    ZHLN::Frustum frustum {};

    DefaultCameraFrustum() {
        ZHLN::Camera cam;
        cam.position = FVec3(0.0f, 0.0f, 10.0f);
        cam.yaw      = -90.0f; // Look towards -Z
        cam.pitch    = 0.0f;
        cam.fov      = 60.0f;
        cam.nearZ    = 0.1f;
        cam.farZ     = 100.0f;

        const auto vp = cam.GetProjectionMatrix(16.0f / 9.0f) * cam.GetViewMatrix();
        frustum.Update(vp);
    }
};

// --- 1. Freestanding Scalar Math ---
void test_scalar_math() {
    static_assert(ZHLN::Abs(-3.5f) == 3.5f && ZHLN::Abs(2.5) == 2.5);
    static_assert(ZHLN::Abs(-7) == 7 && ZHLN::Abs(7u) == 7u);
    static_assert(ZHLN::Abs(static_cast<signed char>(-4)) == 4);
    static_assert(Vec3<int> {-2, 3, -4}.Abs() == Vec3<int> {2, 3, 4});
    static_assert(Vec3<float> {-2.0f, 3.0f, -4.0f}.Abs() == Vec3<float> {2.0f, 3.0f, 4.0f});
    static_assert(Vec4<float> {-2.0f, 3.0f, -4.0f, -5.0f}.Abs() == Vec4<float> {2.0f, 3.0f, 4.0f, 5.0f});
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

// FVec3's component helpers match the existing Math3D FVec3 helpers while
// leaving the old API untouched for callers that have not migrated yet.
void test_vec3_math() {
    constexpr FVec3 a {1.0f, 2.0f, 3.0f};
    constexpr FVec3 b {4.0f, 5.0f, 6.0f};
    static_assert(a + b == FVec3 {5.0f, 7.0f, 9.0f});
    static_assert(b - a == FVec3 {3.0f, 3.0f, 3.0f});
    static_assert(a * b == FVec3 {4.0f, 10.0f, 18.0f});
    static_assert(2.0f * a == FVec3 {2.0f, 4.0f, 6.0f});
    static_assert(b / 2.0f == FVec3 {2.0f, 2.5f, 3.0f});
    static_assert(Dot(a, b) == 32.0f);
    static_assert(Cross(FVec3::UnitX(), FVec3::UnitY()) == FVec3::UnitZ());
    static_assert(a.LengthSquared() == 14.0f);
    static_assert(DistanceSquared(a, b) == 27.0f);
    static_assert(Lerp(a, b, 0.5f) == FVec3 {2.5f, 3.5f, 4.5f});
    static_assert(a[0] == 1.0f && a[1] == 2.0f && a[2] == 3.0f);
    static_assert(sizeof(FVec3) == 3 * sizeof(float));
    static_assert(sizeof(DVec3) == 3 * sizeof(double));
    static_assert(a.Cast<double>() == DVec3 {1.0, 2.0, 3.0});
    static_assert(DVec3 {1.5, -2.25, 3.0}.Cast<float>() == FVec3 {1.5f, -2.25f, 3.0f});
    static_assert(!std::is_convertible_v<DVec3, FVec3>);

    FVec3 mutableVector = a;
    mutableVector[1] = 8.0f;
    mutableVector += b;
    mutableVector -= b;
    mutableVector *= 2.0f;
    mutableVector /= 2.0f;
    Expect(mutableVector.y) == 8.0f;
    Expect(FVec3::Zero().Normalized() == FVec3::Zero()) == true;
    const FVec3 unit = FVec3 {3.0f, 4.0f, 0.0f}.Normalized();
    Expect(std::abs(unit.Length() - 1.0f) < 1e-6f) == true;
    Expect(std::abs(Distance(DVec3 {3.0, 4.0, 0.0}, DVec3::Zero()) - 5.0) < 1e-12) == true;

    const FVec3 p {-1.25f, 2.5f, -4.0f};
    const FVec3 period {2.0f, 3.0f, 5.0f};
    const auto oldFloor = Math::Floor(FVec3 {p.x, p.y, p.z});
    const auto oldFract = Math::Fract(FVec3 {p.x, p.y, p.z});
    const auto oldWrap = Math::Wrap(FVec3 {p.x, p.y, p.z}, FVec3 {period.x, period.y, period.z});
    Expect(p.Floor() == FVec3 {oldFloor.x, oldFloor.y, oldFloor.z}) == true;
    Expect(p.Fract() == FVec3 {oldFract.x, oldFract.y, oldFract.z}) == true;
    Expect(Wrap(p, period) == FVec3 {oldWrap.x, oldWrap.y, oldWrap.z}) == true;
    const DVec3 dp {-1.25, 2.5, -4.0};
    Expect(dp.Fract() == DVec3 {0.75, 0.5, 0.0}) == true;
    Expect(Wrap(dp, DVec3 {2.0, 3.0, 5.0}) == DVec3 {0.75, 2.5, 1.0}) == true;
}

void test_vec4_math() {
    constexpr FVec4 a {1.0f, 2.0f, 3.0f, 4.0f};
    constexpr FVec4 b {4.0f, 5.0f, 6.0f, 7.0f};
    static_assert(FVec4::Zero() == FVec4 {0, 0, 0, 0});
    static_assert(FVec4::One() == FVec4 {1, 1, 1, 1});
    static_assert(FVec4::Splat(2.0f) == FVec4 {2, 2, 2, 2});
    static_assert(FVec4::UnitW() == FVec4 {0, 0, 0, 1});
    static_assert(FVec4::Forward() == FVec4 {0, 0, -1, 0});
    static_assert(-a == FVec4 {-1, -2, -3, -4});
    static_assert(a + b == FVec4 {5, 7, 9, 11});
    static_assert(b - a == FVec4 {3, 3, 3, 3});
    static_assert(a * b == FVec4 {4, 10, 18, 28});
    static_assert(b / a == FVec4 {4, 2.5f, 2, 1.75f});
    static_assert(2.0f * a == FVec4 {2, 4, 6, 8});
    static_assert(a / 2.0f == FVec4 {0.5f, 1, 1.5f, 2});
    static_assert(Dot(a, b) == 60.0f);
    static_assert(a.LengthSquared() == 30.0f);
    static_assert(DistanceSquared(a, b) == 36.0f);
    static_assert(Lerp(a, b, 0.5f) == FVec4 {2.5f, 3.5f, 4.5f, 5.5f});
    static_assert(a[0] == 1 && a[1] == 2 && a[2] == 3 && a[3] == 4);
    static_assert(sizeof(FVec4) == 4 * sizeof(float));
    static_assert(sizeof(DVec4) == 4 * sizeof(double));

    FVec4 v = a;
    v[3] = 8.0f;
    v += b;
    v -= b;
    v *= 2.0f;
    v /= 2.0f;
    Expect(v.w) == 8.0f;
    Expect(FVec4::Zero().Normalized() == FVec4::Zero()) == true;
    Expect(std::abs(FVec4 {1, 2, 2, 4}.Length() - 5.0f) < 1e-6f) == true;
    Expect(std::abs(FVec4 {1, 2, 2, 4}.Normalized().Length() - 1.0f) < 1e-6f) == true;
    Expect(std::abs(Distance(DVec4 {1, 2, 2, 4}, DVec4::Zero()) - 5.0) < 1e-12) == true;
    const DVec4 p {-1.25, 2.5, -4.0, 5.75};
    Expect(p.Floor() == DVec4 {-2, 2, -4, 5}) == true;
    Expect(p.Fract() == DVec4 {0.75, 0.5, 0.0, 0.75}) == true;
    Expect(Wrap(p, DVec4 {2, 3, 5, 2}) == DVec4 {0.75, 2.5, 1, 1.75}) == true;
}

template <typename M>
concept HasInversed = requires(const M& m) { m.Inversed(); };

void test_matrix_inversed_method() {
    using Matrix3 = Matrix<float, 3, 3>;
    using AlignedMatrix4 = Matrix<double, 4, 4, 32>;
    static_assert(HasInversed<Matrix3> && HasInversed<Mat4x4> && HasInversed<AlignedMatrix4>);
    static_assert(!HasInversed<Matrix<float, 3, 4>> && !HasInversed<Matrix<int, 3, 3>>);
    static_assert(Matrix3::Identity().Inversed() == Matrix3::Identity());
    static_assert(Mat4x4::Identity().Inversed() == Mat4x4::Identity());
    static_assert(Matrix3::Zero().Inversed() == Matrix3::Identity());
    static_assert(Mat4x4::Zero().Inversed() == Mat4x4::Identity());
    constexpr auto reflected3 = Matrix3::FromCols({-2.0f, 0.0f, 0.0f}, {0.0f, 4.0f, 0.0f}, {0.0f, 0.0f, 2.0f});
    static_assert(reflected3.Inversed()[0, 0] == -0.5f);
    constexpr auto reflected4 = CreateScale(FVec3 {-2.0f, 4.0f, 2.0f});
    static_assert(reflected4.Inversed()[0, 0] == -0.5f);
    const auto matrix3 = Matrix3::FromCols({2.0f, 0.0f, 0.0f}, {1.0f, 3.0f, 0.0f}, {0.0f, 1.0f, 4.0f});
    Expect(matrix3.Inversed() == Inverse(matrix3)) == true;
    const auto identity3 = matrix3 * matrix3.Inversed();
    for (size_t r = 0; r < 3; ++r) for (size_t c = 0; c < 3; ++c)
        Expect(std::abs(identity3[r, c] - (r == c ? 1.0f : 0.0f)) < 1e-5f) == true;
    const auto matrix4 = CreateTranslation(FVec3 {3.0f, -2.0f, 1.0f}) * CreateRotationY(0.7f) * CreateScale(FVec3 {2.0f, 3.0f, 4.0f});
    Expect(matrix4.Inversed() == Inverse(matrix4)) == true;
    const auto identity4 = matrix4 * matrix4.Inversed();
    for (size_t r = 0; r < 4; ++r) for (size_t c = 0; c < 4; ++c)
        Expect(std::abs(identity4[r, c] - (r == c ? 1.0f : 0.0f)) < 1e-5f) == true;
    Expect(Matrix3::Zero().Inversed() == Matrix3::Identity()) == true;
    auto nearSingular3 = Matrix3::Identity();
    nearSingular3[0, 0] = 1e-9f;
    Expect(nearSingular3.Inversed() == Matrix3::Identity()) == true;
    auto nearSingular4 = Mat4x4::Identity();
    nearSingular4[0, 0] = 1e-9f;
    Expect(nearSingular4.Inversed() == Mat4x4::Identity()) == true;
    Expect(Mat4x4::Zero().Inversed() == Mat4x4::Identity()) == true;
    Expect(AlignedMatrix4::Identity().Inversed() == AlignedMatrix4::Identity()) == true;
}

void test_picking_unprojection_parity() {
    Camera camera;
    camera.position = {3.0f, -2.0f, 7.0f};
    camera.yaw = -70.0f;
    camera.pitch = 20.0f;
    camera.fov = 63.0f;
    camera.nearZ = 0.1f;
    camera.farZ = 200.0f;
    constexpr float aspect = 1.6f;
    const auto inverseViewProjection = (camera.GetProjectionMatrix(aspect) * camera.GetViewMatrix()).Inversed();
    const auto yaw = DegF(camera.yaw).ToRadians();
    const auto pitch = DegF(camera.pitch).ToRadians();
    const FVec3 forward {Cos(yaw) * Cos(pitch), Sin(pitch), Sin(yaw) * Cos(pitch)};
    const auto oldView = Math::CreateLookAt(camera.position, camera.position + forward, FVec3::UnitY());
    const auto oldProjection = Math::CreatePerspective(DegF(camera.fov).ToRadians().value, aspect, camera.nearZ, camera.farZ);
    const auto oldInverse = (oldProjection * oldView).Inversed();
    // The editor's viewport has a nonzero offset, so mouse coordinates are
    // relative to the viewport before mapping to Vulkan's Y-down NDC.
    for (const float mouseX: {180.0f, 500.0f, 820.0f}) {
        for (const float mouseY: {90.0f, 250.0f, 410.0f}) {
            const float x = 2.0f * (mouseX - 100.0f) / 800.0f - 1.0f;
            const float y = 2.0f * (mouseY - 50.0f) / 400.0f - 1.0f;
            const auto n = inverseViewProjection * FVec4 {x, y, 0.0f, 1.0f};
            const auto f = inverseViewProjection * FVec4 {x, y, 1.0f, 1.0f};
            const FVec3 nearPoint = n.XYZ() / n.w;
            const FVec3 direction = (f.XYZ() / f.w - nearPoint).Normalized();
            const auto oldNear = oldInverse * JPH::Vec4(x, y, 0.0f, 1.0f);
            const auto oldFar = oldInverse * JPH::Vec4(x, y, 1.0f, 1.0f);
            const JPH::Vec3 jNear = JPH::Vec3(oldNear.GetX(), oldNear.GetY(), oldNear.GetZ()) / oldNear.GetW();
            const JPH::Vec3 jDir = (JPH::Vec3(oldFar.GetX(), oldFar.GetY(), oldFar.GetZ()) / oldFar.GetW() - jNear).Normalized();
            Expect((nearPoint - FVec3 {jNear.GetX(), jNear.GetY(), jNear.GetZ()}).Length() < 1e-4f) == true;
            Expect((direction - FVec3 {jDir.GetX(), jDir.GetY(), jDir.GetZ()}).Length() < 1e-4f) == true;
            if (x == 0.0f && y == 0.0f) Expect((direction - forward).Length() < 1e-4f) == true;
        }
    }
}

// Camera matrices and extracted frustum planes must preserve the legacy Jolt conventions.
void test_native_camera_matrix_parity() {
    Camera cam;
    cam.position = {3.0f, -2.0f, 7.0f};
    cam.fov = 63.0f;
    cam.nearZ = 0.1f;
    cam.farZ = 200.0f;
    for (const float yaw: {-170.0f, -90.0f, 0.0f, 45.0f, 120.0f}) {
        for (const float pitch: {-60.0f, 0.0f, 50.0f}) {
            cam.yaw = yaw;
            cam.pitch = pitch;
            const auto view = cam.GetViewMatrix();
            const auto projection = cam.GetProjectionMatrix(1.6f);
            const RadF yawRad = DegF(cam.yaw).ToRadians();
            const RadF pitchRad = DegF(cam.pitch).ToRadians();
            const FVec3 direction {Cos(yawRad) * Cos(pitchRad), Sin(pitchRad), Sin(yawRad) * Cos(pitchRad)};
            const auto oldView = Math::CreateLookAt(cam.position, cam.position + direction.Normalized(), FVec3::UnitY());
            const auto oldProjection = Math::CreatePerspective(DegF(cam.fov).ToRadians().value, 1.6f, cam.nearZ, cam.farZ);
            for (int column = 0; column < 4; ++column) {
                for (int row = 0; row < 4; ++row) {
                    const auto r = static_cast<size_t>(row);
                    const auto c = static_cast<size_t>(column);
                    Expect(std::abs(view[r, c] - oldView(row, column)) < 1e-4f) == true;
                    Expect(std::abs(projection[r, c] - oldProjection(row, column)) < 1e-4f) == true;
                }
            }
            Frustum nativeFrustum;
            nativeFrustum.Update(projection * view);
            const auto oldVP = oldProjection * oldView;
            // Independent reference: extract and normalize the six planes from
            // the Jolt product, without routing through Frustum::Update.
            float planes[6][4] {};
            for (int column = 0; column < 4; ++column) {
                planes[0][column] = oldVP(3, column) + oldVP(0, column);
                planes[1][column] = oldVP(3, column) - oldVP(0, column);
                planes[2][column] = oldVP(3, column) + oldVP(1, column);
                planes[3][column] = oldVP(3, column) - oldVP(1, column);
                planes[4][column] = oldVP(2, column);
                planes[5][column] = oldVP(3, column) - oldVP(2, column);
            }
            for (auto& plane: planes) {
                const float length = std::sqrt(plane[0] * plane[0] + plane[1] * plane[1] + plane[2] * plane[2]);
                if (length > 1e-6f) {
                    for (float& component: plane) component /= length;
                }
            }
            for (const float x: {-10.0f, 0.0f, 10.0f}) {
                for (const float z: {-10.0f, 0.0f, 10.0f}) {
                    bool referenceVisible = true;
                    for (const auto& plane: planes) {
                        if (plane[0] * x + plane[2] * z + plane[3] < -1.5f) referenceVisible = false;
                    }
                    Expect(nativeFrustum.IsSphereVisible({x, 0.0f, z}, 1.0f)) == referenceVisible;
                }
            }
        }
    }
}

// Compare native prefab transform math with the legacy Jolt implementation,
// including reflections, degenerate scale, and sheared parent/child products.
void test_native_transform_parity() {
    for (const float angle: {-3.0f, -1.5f, 0.0f, 1.1f, 2.8f}) {
        const auto jq = JPH::Quat::sEulerAngles(JPH::Vec3(angle, 0.37f, -0.61f));
        const FVec4 rotation {jq.GetX(), jq.GetY(), jq.GetZ(), jq.GetW()};
        for (const auto scale: {FVec3 {1.0f, 1.0f, 1.0f}, FVec3 {2.0f, 3.0f, 0.5f}, FVec3 {-2.0f, 3.0f, 4.0f},
                                FVec3 {0.0f, 1.0f, 2.0f}, FVec3 {1.0f, 0.0f, 0.0f}}) {
            const auto matrix = ComposeTransform({3.0f, -4.0f, 7.0f}, rotation, scale);
            const auto reference = Math::CreateTransform({3.0f, -4.0f, 7.0f}, rotation, scale);
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    Expect(std::abs(matrix[static_cast<size_t>(row), static_cast<size_t>(col)] - reference(row, col)) < 1e-4f) == true;
                }
            }
            for (const bool sheared: {false, true}) {
                auto input = matrix;
                if (sheared) input[0, 1] += 0.25f;
                const auto joltInput = JPH::Mat44(
                    JPH::Vec4(input[0, 0], input[1, 0], input[2, 0], input[3, 0]),
                    JPH::Vec4(input[0, 1], input[1, 1], input[2, 1], input[3, 1]),
                    JPH::Vec4(input[0, 2], input[1, 2], input[2, 2], input[3, 2]),
                    JPH::Vec4(input[0, 3], input[1, 3], input[2, 3], input[3, 3]));
                const auto native = DecomposeTransform(input);
                const auto legacy = Math::Decompose(joltInput);
                Expect(std::abs(native.translation.x - legacy.translation.GetX()) < 1e-4f) == true;
                Expect(std::abs(native.translation.y - legacy.translation.GetY()) < 1e-4f) == true;
                Expect(std::abs(native.translation.z - legacy.translation.GetZ()) < 1e-4f) == true;
                Expect(std::abs(native.scale.x - legacy.scale.GetX()) < 1e-4f) == true;
                Expect(std::abs(native.scale.y - legacy.scale.GetY()) < 1e-4f) == true;
                Expect(std::abs(native.scale.z - legacy.scale.GetZ()) < 1e-4f) == true;
                const auto alignment = std::abs(native.rotation.x * legacy.rotation.GetX() + native.rotation.y * legacy.rotation.GetY() +
                                                native.rotation.z * legacy.rotation.GetZ() + native.rotation.w * legacy.rotation.GetW());
                Expect(std::abs(alignment - 1.0f) < 1e-4f) == true;
            }
        }
    }
}

// Rotation interpolation and scene Euler conversion retain Jolt conventions.
void test_native_rotation_parity() {
    for (const float x: {-135.0f, -20.0f, 0.0f, 75.0f, 170.0f}) {
        for (const float y: {-70.0f, 0.0f, 60.0f}) {
            const FVec3 angles {x, y, 38.0f};
            const auto native = RotationFromEulerDegrees(angles);
            const auto legacy = Math::EulerDegreesToQuat(angles);
            Expect(std::abs(native.x - legacy.GetX()) < 1e-5f) == true;
            Expect(std::abs(native.y - legacy.GetY()) < 1e-5f) == true;
            Expect(std::abs(native.z - legacy.GetZ()) < 1e-5f) == true;
            Expect(std::abs(native.w - legacy.GetW()) < 1e-5f) == true;
            const auto nativeEuler = EulerDegreesFromRotation(native);
            const auto legacyEuler = legacy.GetEulerAngles() * (180.0f / JPH::JPH_PI);
            Expect(std::abs(nativeEuler.x - legacyEuler.GetX()) < 1e-3f) == true;
            Expect(std::abs(nativeEuler.y - legacyEuler.GetY()) < 1e-3f) == true;
            Expect(std::abs(nativeEuler.z - legacyEuler.GetZ()) < 1e-3f) == true;

            const auto other = RotationFromEulerDegrees({-x, 15.0f, -80.0f});
            const auto otherLegacy = JPH::Quat(other.x, other.y, other.z, other.w);
            const auto product = MultiplyRotations(native, other);
            const auto referenceProduct = legacy * otherLegacy;
            Expect(std::abs(product.x - referenceProduct.GetX()) < 1e-5f) == true;
            Expect(std::abs(product.y - referenceProduct.GetY()) < 1e-5f) == true;
            Expect(std::abs(product.z - referenceProduct.GetZ()) < 1e-5f) == true;
            Expect(std::abs(product.w - referenceProduct.GetW()) < 1e-5f) == true;
            const auto identity = MultiplyRotations(native, ConjugateRotation(native));
            Expect(std::abs(identity.w - 1.0f) < 1e-5f) == true;
            for (const float t: {0.0f, 0.1f, 0.5f, 0.9f, 1.0f}) {
                const auto blended = SlerpRotation(native, other, t);
                const auto reference = legacy.SLERP(otherLegacy, t);
                const auto alignment = std::abs(blended.x * reference.GetX() + blended.y * reference.GetY() +
                                                blended.z * reference.GetZ() + blended.w * reference.GetW());
                Expect(std::abs(alignment - 1.0f) < 1e-4f) == true;
                // q and -q encode the same rotation and must choose the short arc.
                const auto antipodal = SlerpRotation(native, -native, t);
                Expect(std::abs(std::abs(Dot(native, antipodal)) - 1.0f) < 1e-4f) == true;
            }
        }
    }
}

void test_vector_abs_and_is_close() {
    constexpr FVec3 a {-2.0f, 3.0f, -4.0f};
    constexpr FVec4 b {-2.0f, 3.0f, -4.0f, -5.0f};
    static_assert(Vec4<int> {-2, 3, -4, -5}.Abs() == Vec4<int> {2, 3, 4, 5});
    static_assert(Vec4<unsigned> {2, 3, 4, 5}.Abs() == Vec4<unsigned> {2, 3, 4, 5});
    Expect(a.Abs() == FVec3 {2.0f, 3.0f, 4.0f}) == true;
    Expect(b.Abs() == FVec4 {2.0f, 3.0f, 4.0f, 5.0f}) == true;
    Expect(a.IsClose(a + FVec3 {0.0f, 0.0f, 0.05f}, 0.01f)) == true;
    Expect(a.IsClose(a + FVec3 {0.0f, 0.0f, 0.2f}, 0.01f)) == false;
    Expect(b.IsClose(b + FVec4 {0.0f, 0.0f, 0.0f, 0.05f}, 0.01f)) == true;
    Expect(b.IsClose(b + FVec4 {0.0f, 0.0f, 0.0f, 0.2f}, 0.01f)) == false;
    Expect((DVec3 {0.0, 0.0, 0.0}).IsClose({0.1, 0.0, 0.0}, 0.011)) == true;
}

void test_transform_component_uses_quaternion() {
    Components::TransformComponent transform;
    static_assert(std::is_same_v<decltype(transform.rotation), Quat>);
    Expect(transform.rotation == Quat::Identity()) == true;
    transform.position = {3.0f, -2.0f, 1.0f};
    transform.rotation = Quat::FromAxisAngle(FVec3::UnitY(), std::numbers::pi_v<float> * 0.5f);
    transform.scale = {2.0f, 1.0f, 1.0f};
    const auto matrix = transform.GetLocalMatrix();
    const auto transformed = (matrix * FVec4 {1.0f, 0.0f, 0.0f, 1.0f}).XYZ();
    Expect((transformed - FVec3 {3.0f, -2.0f, -1.0f}).Length() < 1e-5f) == true;
    Expect(transform.GetMatrix() == matrix) == true;
}

void test_quaternion_math() {
    constexpr auto identity = Quat::Identity();
    static_assert(sizeof(Quat) == 16 && alignof(Quat) == 16);
    static_assert(identity == Quat {0.0f, 0.0f, 0.0f, 1.0f});
    static_assert(Dot(identity, identity) == 1.0f);
    static_assert(Rotate(identity, FVec3::UnitX()) == FVec3::UnitX());
    const auto quarterTurn = Quat::FromAxisAngle(FVec3::UnitY(), std::numbers::pi_v<float> * 0.5f);
    const auto rotated = Rotate(quarterTurn, FVec3::UnitX());
    Expect((quarterTurn * FVec3::UnitX() - rotated).Length() < 1e-6f) == true;
    FVec3 axis;
    float angle = 0.0f;
    quarterTurn.ToAxisAngle(axis, angle);
    Expect((axis - FVec3::UnitY()).Length() < 1e-6f && std::abs(angle - std::numbers::pi_v<float> * 0.5f) < 1e-6f) == true;
    FVec3 negativeAxis;
    float negativeAngle = 0.0f;
    (-quarterTurn).ToAxisAngle(negativeAxis, negativeAngle);
    Expect((negativeAxis - axis).Length() < 1e-6f && std::abs(negativeAngle - angle) < 1e-6f) == true;
    identity.ToAxisAngle(axis, angle);
    Expect(axis == FVec3::Zero() && angle == 0.0f) == true;
    Expect(std::abs(rotated.x) < 1e-5f && std::abs(rotated.z + 1.0f) < 1e-5f) == true;
    const auto undone = Rotate(quarterTurn.Inversed(), rotated);
    Expect((undone - FVec3::UnitX()).Length() < 1e-5f) == true;
    const auto opposite = FromTo(FVec3::UnitX(), -FVec3::UnitX());
    Expect((Rotate(opposite, FVec3::UnitX()) + FVec3::UnitX()).Length() < 1e-5f) == true;
    Expect(FromTo(FVec3::Zero(), FVec3::UnitX()) == identity) == true;
    Expect(Quat {0.0f, 0.0f, 0.0f, 0.0f}.Normalized() == identity) == true;
    const auto half = Slerp(identity, quarterTurn, 0.5f);
    const auto same = Slerp(identity, -quarterTurn, 0.5f);
    Expect(std::abs(std::abs(Dot(half, same)) - 1.0f) < 1e-5f) == true;
    const auto euler = Quat::FromEulerDegrees({20.0f, 30.0f, -45.0f});
    const auto degrees = euler.EulerAnglesDegrees();
    Expect((degrees - FVec3 {20.0f, 30.0f, -45.0f}).Length() < 1e-4f) == true;
    static_assert(std::is_same_v<decltype(TransformParts{}.rotation), Quat>);
    const auto reference = Math::EulerDegreesToQuat({20.0f, 30.0f, -45.0f});
    Expect(std::abs(euler.x - reference.GetX()) < 1e-5f && std::abs(euler.y - reference.GetY()) < 1e-5f &&
           std::abs(euler.z - reference.GetZ()) < 1e-5f && std::abs(euler.w - reference.GetW()) < 1e-5f) == true;
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
    Expect(camFrustum.frustum.IsSphereVisible({x, y, z}, 1.0f)) == expectedVisible;
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
