// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Math3D.hpp"

namespace ZHLN {

struct Frustum {
    // SIMD SoA Layout
    JPH::Vec4 mX[2], mY[2], mZ[2], mW[2];

    void Update(const JPH::Mat44& vp) {
        // 1. Manually extract rows from the Column-Major matrix
        // vp(row, column)
        JPH::Vec4 r0(vp(0, 0), vp(0, 1), vp(0, 2), vp(0, 3));
        JPH::Vec4 r1(vp(1, 0), vp(1, 1), vp(1, 2), vp(1, 3));
        JPH::Vec4 r2(vp(2, 0), vp(2, 1), vp(2, 2), vp(2, 3));
        JPH::Vec4 r3(vp(3, 0), vp(3, 1), vp(3, 2), vp(3, 3));

        JPH::Vec4 planes[6];
        // Left/Right
        planes[0] = r3 + r0;
        planes[1] = r3 - r0;
        // Top/Bottom (Vulkan Y is Down, so r3 + r1 is Top)
        planes[2] = r3 + r1;
        planes[3] = r3 - r1;
        // Near/Far (Vulkan Z is 0..1)
        planes[4] = r2;
        planes[5] = r3 - r2;

        for (auto& plane: planes) {
            // Normalize planes to ensure distance checks are in world units
            float len = JPH::Vec3(plane.GetX(), plane.GetY(), plane.GetZ()).Length();
            if (len > 1e-6f) {
                plane /= len;
            }
        }

        // 2. Transpose to SIMD SoA
        mX[0] = JPH::Vec4(planes[0].GetX(), planes[1].GetX(), planes[2].GetX(), planes[3].GetX());
        mY[0] = JPH::Vec4(planes[0].GetY(), planes[1].GetY(), planes[2].GetY(), planes[3].GetY());
        mZ[0] = JPH::Vec4(planes[0].GetZ(), planes[1].GetZ(), planes[2].GetZ(), planes[3].GetZ());
        mW[0] = JPH::Vec4(planes[0].GetW(), planes[1].GetW(), planes[2].GetW(), planes[3].GetW());

        mX[1] = JPH::Vec4(planes[4].GetX(), planes[5].GetX(), 0.0f, 0.0f);
        mY[1] = JPH::Vec4(planes[4].GetY(), planes[5].GetY(), 0.0f, 0.0f);
        mZ[1] = JPH::Vec4(planes[4].GetZ(), planes[5].GetZ(), 0.0f, 0.0f);
        // Lane 3 & 4 of block 1 are "always true" planes (W = large positive)
        mW[1] = JPH::Vec4(planes[4].GetW(), planes[5].GetW(), 1e10f, 1e10f);
    }

    [[nodiscard]] JPH_INLINE bool IsSphereVisible(JPH::Vec3Arg center, float radius) const {
        // Stability Fix: Inflate radius by a small margin (0.5m)
        // This prevents "flicker" culling which causes renderer command spikes
        float inflatedRadius = -(radius + 0.5f);

        JPH::Vec4 cX   = JPH::Vec4::sReplicate(center.GetX());
        JPH::Vec4 cY   = JPH::Vec4::sReplicate(center.GetY());
        JPH::Vec4 cZ   = JPH::Vec4::sReplicate(center.GetZ());
        JPH::Vec4 negR = JPH::Vec4::sReplicate(inflatedRadius);

        // block 0 (Planes 0-3)
        JPH::Vec4 dist0 = mX[0] * cX + mY[0] * cY + mZ[0] * cZ + mW[0];
        if (JPH::Vec4::sLess(dist0, negR).TestAnyTrue()) {
            return false;
        }

        // block 1 (Planes 4-5)
        JPH::Vec4 dist1 = mX[1] * cX + mY[1] * cY + mZ[1] * cZ + mW[1];
        return !JPH::Vec4::sLess(dist1, negR).TestAnyTrue();
    }
};

struct Camera {
    JPH::Vec3 position = {0, 2, 10};
    float     yaw      = -90.0f;
    float     pitch    = 0.0f;

    float fov   = 45.0f;
    float nearZ = 0.1f;
    float farZ  = 1000.0f;

    Frustum frustum {};
    Frustum shadowFrustum {};

    [[nodiscard]] JPH::Mat44 GetViewMatrix() const {
        JPH::Vec3 direction {};
        direction.SetX(JPH::Cos(JPH::DegreesToRadians(yaw)) * JPH::Cos(JPH::DegreesToRadians(pitch)));
        direction.SetY(JPH::Sin(JPH::DegreesToRadians(pitch)));
        direction.SetZ(JPH::Sin(JPH::DegreesToRadians(yaw)) * JPH::Cos(JPH::DegreesToRadians(pitch)));

        return Math::CreateLookAt(position, position + direction.Normalized(), JPH::Vec3::sAxisY());
    }

    [[nodiscard]] JPH::Mat44 GetProjectionMatrix(float aspectRatio) const {
        return Math::CreatePerspective(JPH::DegreesToRadians(fov), aspectRatio, nearZ, farZ);
    }

    static constexpr float Halton_2[16] = {0.5f,    0.25f,   0.75f,   0.125f,  0.625f,  0.375f,  0.875f,  0.0625f,
                                           0.5625f, 0.3125f, 0.8125f, 0.1875f, 0.6875f, 0.4375f, 0.9375f, 0.03125f};
    static constexpr float Halton_3[16] = {0.333f, 0.666f, 0.111f, 0.444f, 0.777f, 0.222f, 0.555f, 0.888f,
                                           0.037f, 0.370f, 0.703f, 0.148f, 0.481f, 0.814f, 0.259f, 0.592f};

    [[nodiscard]] JPH::Mat44 GetJitteredProjectionMatrix(float aspectRatio, uint32_t width, uint32_t height, AAState& aaState) const {
        JPH::Mat44 proj = GetProjectionMatrix(aspectRatio);

        if (aaState.mode == AAMode::TAA) {
            // Map Halton sequence [-0.5, 0.5] to Sub-Pixel NDC space
            float jitterX = (Halton_2[aaState.frameIndex % 16] - 0.5f) / (float) width;
            float jitterY = (Halton_3[aaState.frameIndex % 16] - 0.5f) / (float) height;

            // If frameIndex is 0, there is no previous jitter
            float prevJitterX = aaState.frameIndex > 0 ? (Camera::Halton_2[(aaState.frameIndex - 1) % 16] - 0.5f) / (float) width : 0.0f;
            float prevJitterY = aaState.frameIndex > 0 ? (Camera::Halton_3[(aaState.frameIndex - 1) % 16] - 0.5f) / (float) height : 0.0f;

            aaState.jitterX     = jitterX;
            aaState.jitterY     = jitterY;
            aaState.prevJitterX = prevJitterX;
            aaState.prevJitterY = prevJitterY;

            // Native Vulkan Y-Down: Positive jitter shifts image correctly
            JPH::Vec4 col2 = proj.GetColumn4(2);
            proj.SetColumn4(2, col2 + JPH::Vec4(jitterX * 2.0f, jitterY * 2.0f, 0.0f, 0.0f));
        }
        return proj;
    }
};

} // namespace ZHLN
