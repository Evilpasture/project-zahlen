// Copyright (C) 2026 Evilpasture | evilpasture+github@proton.me
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <Zahlen/Math/Angles.hpp>
#include <Zahlen/Math/Matrix.hpp>
#include <Zahlen/Math/Vec3.hpp>
#include <Zahlen/Math/Vec4.hpp>
#include <array>

namespace ZHLN {

struct Frustum {
    std::array<FVec4, 2> mX {}, mY {}, mZ {}, mW {};

    void Update(const Mat4x4& vp) {
        FVec4 r0(vp[0, 0], vp[0, 1], vp[0, 2], vp[0, 3]);
        FVec4 r1(vp[1, 0], vp[1, 1], vp[1, 2], vp[1, 3]);
        FVec4 r2(vp[2, 0], vp[2, 1], vp[2, 2], vp[2, 3]);
        FVec4 r3(vp[3, 0], vp[3, 1], vp[3, 2], vp[3, 3]);

        std::array<FVec4, 6> planes {};
        planes[0] = r3 + r0;
        planes[1] = r3 - r0;
        planes[2] = r3 + r1;
        planes[3] = r3 - r1;
        planes[4] = r2;
        planes[5] = r3 - r2;

        for (auto& plane: planes) {
            float len = FVec3(plane.x, plane.y, plane.z).Length();
            if (len > 1e-6f) {
                plane /= len;
            }
        }

        mX[0] = FVec4(planes[0].x, planes[1].x, planes[2].x, planes[3].x);
        mY[0] = FVec4(planes[0].y, planes[1].y, planes[2].y, planes[3].y);
        mZ[0] = FVec4(planes[0].z, planes[1].z, planes[2].z, planes[3].z);
        mW[0] = FVec4(planes[0].w, planes[1].w, planes[2].w, planes[3].w);

        mX[1] = FVec4(planes[4].x, planes[5].x, 0.0f, 0.0f);
        mY[1] = FVec4(planes[4].y, planes[5].y, 0.0f, 0.0f);
        mZ[1] = FVec4(planes[4].z, planes[5].z, 0.0f, 0.0f);
        mW[1] = FVec4(planes[4].w, planes[5].w, 1e10f, 1e10f);
    }

    [[nodiscard]] auto IsSphereVisible(FVec3 center, float radius) const -> bool {
        const float inflatedRadius = -(radius + 0.5f);

        FVec4 cX   = FVec4::Splat(center.x);
        FVec4 cY   = FVec4::Splat(center.y);
        FVec4 cZ   = FVec4::Splat(center.z);
        FVec4 negR = FVec4::Splat(inflatedRadius);

        FVec4 dist0 = mX[0] * cX + mY[0] * cY + mZ[0] * cZ + mW[0];
        if (dist0.AnyLess(negR)) {
            return false;
        }

        FVec4 dist1 = mX[1] * cX + mY[1] * cY + mZ[1] * cZ + mW[1];
        return !dist1.AnyLess(negR);
    }
};

struct Camera {
    FVec3 position = {0.0f, 2.0f, 10.0f};
    float yaw      = -90.0f;
    float pitch    = 0.0f;

    float fov   = 45.0f;
    float nearZ = 0.1f;
    float farZ  = 1000.0f;

    [[nodiscard]] auto GetViewMatrix() const -> Mat4x4 {
        const RadF pitchRad = DegF(pitch).ToRadians();
        const RadF yawRad   = DegF(yaw).ToRadians();
        const FVec3 direction {Cos(yawRad) * Cos(pitchRad), Sin(pitchRad), Sin(yawRad) * Cos(pitchRad)};
        return CreateLookAt(position, position + direction.Normalized(), FVec3::UnitY());
    }

    [[nodiscard]] auto GetProjectionMatrix(float aspectRatio) const -> Mat4x4 {
        return CreatePerspective(DegF(fov).ToRadians().value, aspectRatio, nearZ, farZ);
    }
};

} // namespace ZHLN
